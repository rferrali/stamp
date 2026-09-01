/*
 * Copyright 2025-2026 Jan-Michael Brummer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "stamp-conversation-row.h"

#include <glib/gi18n.h>

#include "stamp-account.h"
#include "stamp-category.h"
#include "stamp-helper.h"
#include "stamp-session.h"
#include "stamp-time-helpers.h"

struct _StampConversationRow {
  GtkBox parent_instance;

  GtkStack *stack;
  GtkCheckButton *check_button;
  AdwAvatar *avatar;
  GtkInscription *participants;
  GtkInscription *topic;
  GtkLabel *date;
  GtkInscription *body;
  GtkLabel *counter;
  GtkImage *flagged_icon;
  GtkBox *left_action;
  GtkLabel *left_action_label;
  GtkImage *left_action_image;
  GtkBox *right_action;
  GtkGrid *row;
  GtkImage *attachment_icon;
  GtkImage *reply_icon;
  GtkImage *forwarded_icon;
  GtkImage *calendar_icon;
  GtkFlowBox *labels;

  GCancellable *cancellable;
  gboolean selected;
  gboolean claimed;
  StampConversationItem *item;
  GPtrArray *bindings;
  guint instance_id;
  guint generation;
  gboolean unread;

  StampAccount *account;
};

G_DEFINE_FINAL_TYPE (StampConversationRow, stamp_conversation_row, GTK_TYPE_BOX);

static guint next_instance_id = 1;

typedef enum {
  PROP_SELECTED = 1,
  PROP_IMPORTANT,
  PROP_UNREAD,
} StampConversationRowProps;

static GParamSpec *properties[PROP_UNREAD + 1];

enum {
  MARK_READ,
  MARK_UNREAD,
  TRASH,
  LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = { 0 };

static void
stamp_conversation_row_dispose (GObject *object)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (object);

  g_ptr_array_remove_range (self->bindings, 0, self->bindings->len);
  g_clear_pointer (&self->bindings, g_ptr_array_unref);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->item);
  g_clear_object (&self->account);

  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_CONVERSATION_ROW);

  G_OBJECT_CLASS (stamp_conversation_row_parent_class)->dispose (object);
}

static void
stamp_conversation_row_get_property (GObject    *object,
                                     guint       property_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (object);

  switch ((StampConversationRowProps)property_id) {
    case PROP_SELECTED:
      g_value_set_boolean (value, self->selected);
      break;
    case PROP_IMPORTANT:
      break;
    case PROP_UNREAD:
      break;
  }
}

static void
set_unread_status (StampConversationRow *self,
                   gboolean              unread)
{
  self->unread = unread;

  if (!unread) {
    gtk_widget_add_css_class (GTK_WIDGET (self->body), "dim-label");
    gtk_widget_remove_css_class (GTK_WIDGET (self->topic), "caption-heading");
    gtk_widget_add_css_class (GTK_WIDGET (self->topic), "caption");
    gtk_widget_remove_css_class (GTK_WIDGET (self->participants), "heading");
    gtk_widget_remove_css_class (GTK_WIDGET (self->topic), "accent");

    gtk_label_set_text (self->left_action_label, _("Mark Unread"));
    gtk_image_set_from_icon_name (self->left_action_image, "mail-unread-symbolic");
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self->topic), "caption");
    gtk_widget_remove_css_class (GTK_WIDGET (self->body), "dim-label");
    gtk_widget_add_css_class (GTK_WIDGET (self->topic), "caption-heading");
    gtk_widget_add_css_class (GTK_WIDGET (self->participants), "heading");
    gtk_widget_add_css_class (GTK_WIDGET (self->topic), "accent");

    gtk_label_set_text (self->left_action_label, _("Mark Read"));
    gtk_image_set_from_icon_name (self->left_action_image, "mark-read-symbolic");
  }
}

static void
stamp_conversation_row_set_property (GObject      *object,
                                     guint         property_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (object);

  switch ((StampConversationRowProps)property_id) {
    case PROP_SELECTED:
      self->selected = g_value_get_boolean (value);
      gtk_check_button_set_active (self->check_button, self->selected);
      break;
    case PROP_IMPORTANT:
      if (g_value_get_boolean (value)) {
        gtk_widget_add_css_class (GTK_WIDGET (self), "warning");
      } else {
        gtk_widget_remove_css_class (GTK_WIDGET (self), "warning");
      }
      break;
    case PROP_UNREAD:
      set_unread_status (self, g_value_get_boolean (value));
      break;
  }
}

static void
set_offset (GtkWidget *row,
            gdouble    offset)
{
  gtk_widget_set_margin_start (row, offset);
  gtk_widget_set_margin_end (row, -offset);
}

static void
on_drag_update (GtkGestureDrag *gesture,
                gdouble         dx,
                gdouble         dy,
                gpointer        user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);
  gdouble pos;

  if (!self->claimed) {
    if (fabs (dx) < 10 || fabs (dx) <= fabs (dy))
      return;

    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    self->claimed = TRUE;
  }

  pos = CLAMP (dx, -96, 96);

  if (pos > 0) {
    gtk_widget_set_opacity (GTK_WIDGET (self->left_action), MIN (pos / 80.0, 1.0));
    gtk_widget_set_opacity (GTK_WIDGET (self->right_action), 0);
  } else {
    gtk_widget_set_opacity (GTK_WIDGET (self->right_action), MIN (-pos / 80.0, 1.0));
    gtk_widget_set_opacity (GTK_WIDGET (self->left_action), 0);
  }

  set_offset (GTK_WIDGET (self->row), pos);
}

static void
animate_to_zero (StampConversationRow *self,
                 gdouble               pos)
{
  AdwAnimationTarget *target = adw_callback_animation_target_new ((AdwAnimationTargetFunc)set_offset, self->row, NULL);
  g_autoptr (AdwAnimation) anim = adw_timed_animation_new (GTK_WIDGET (self->row), pos, 0, 200, target);
  adw_animation_play (anim);
}

static void
on_drag_end (GtkGestureDrag *gesture,
             gdouble         dx,
             gdouble         dy,
             gpointer        user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);
  gdouble pos;

  pos = CLAMP (dx, -96, 96);

  self->claimed = FALSE;

  if (pos > 90)
    if (self->unread)
      g_signal_emit (self, signals[MARK_READ], 0);
    else
      g_signal_emit (self, signals[MARK_UNREAD], 0);
  else if (pos < -90)
    g_signal_emit (self, signals[TRASH], 0);

  animate_to_zero (self, pos);
  gtk_widget_set_opacity (GTK_WIDGET (self->row), 1);
  gtk_widget_set_opacity (GTK_WIDGET (self->left_action), 0);
  gtk_widget_set_opacity (GTK_WIDGET (self->right_action), 0);
}

static void
on_check_button_toggled (GtkCheckButton *check,
                         gpointer        user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);

  self->selected = gtk_check_button_get_active (check);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SELECTED]);
}

void
stamp_conversation_row_class_init (StampConversationRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = stamp_conversation_row_dispose;
  object_class->get_property = stamp_conversation_row_get_property;
  object_class->set_property = stamp_conversation_row_set_property;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/conversation-list/stamp-conversation-row.ui");
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, stack);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, check_button);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, avatar);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, participants);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, date);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, topic);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, body);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, counter);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, flagged_icon);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, left_action);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, left_action_label);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, left_action_image);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, right_action);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, row);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, attachment_icon);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, reply_icon);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, forwarded_icon);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, calendar_icon);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, labels);

  gtk_widget_class_bind_template_callback (widget_class, on_drag_update);
  gtk_widget_class_bind_template_callback (widget_class, on_drag_end);
  gtk_widget_class_bind_template_callback (widget_class, on_check_button_toggled);

  properties[PROP_SELECTED] = g_param_spec_boolean ("selected",
                                                    NULL,
                                                    NULL,
                                                    FALSE,
                                                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_IMPORTANT] = g_param_spec_boolean ("important",
                                                     NULL,
                                                     NULL,
                                                     FALSE,
                                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_UNREAD] = g_param_spec_boolean ("unread",
                                                  NULL,
                                                  NULL,
                                                  FALSE,
                                                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);

  signals[MARK_READ] = g_signal_new ("mark-read", G_OBJECT_CLASS_TYPE (klass),
                                     G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE,
                                     0);
  signals[MARK_UNREAD] = g_signal_new ("mark-unread", G_OBJECT_CLASS_TYPE (klass),
                                       G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                       0, NULL, NULL, NULL,
                                       G_TYPE_NONE,
                                       0);
  signals[TRASH] = g_signal_new ("trash", G_OBJECT_CLASS_TYPE (klass),
                                 G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                 0, NULL, NULL, NULL,
                                 G_TYPE_NONE,
                                 0);
}

void
stamp_conversation_row_init (StampConversationRow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->bindings = g_ptr_array_new_with_free_func ((GDestroyNotify)g_binding_unbind);
  self->instance_id = next_instance_id++;
}

GtkWidget *
stamp_conversation_row_new (void)
{
  return g_object_new (STAMP_TYPE_CONVERSATION_ROW, NULL);
}


typedef struct {
  StampConversationRow *self;
  guint instance_id;
  guint generation;
} StampPhotoToken;

static void
on_get_photo (gpointer texture,
              gpointer user_data)
{
  StampPhotoToken *token = user_data;
  StampConversationRow *self = STAMP_CONVERSATION_ROW (token->self);

  if (token->instance_id != self->instance_id || token->generation != token->self->generation) {
    if (texture)
      g_object_unref (texture);

    g_free (token);
    return;
  }

  if (texture)
    adw_avatar_set_custom_image (ADW_AVATAR (self->avatar), GDK_PAINTABLE (texture));
  else
    adw_avatar_set_custom_image (ADW_AVATAR (self->avatar), NULL);

  g_free (token);
}

static gboolean
transform_flagged_to (GBinding     *binding,
                      const GValue *from_value,
                      GValue       *to_value,
                      gpointer      user_data)
{
  gint flagged = g_value_get_boolean (from_value);

  if (flagged)
    g_value_set_static_string (to_value, "starred-symbolic");
  else
    g_value_set_static_string (to_value, "non-starred-symbolic");

  return TRUE;
}

static gboolean
transfer_date_to (GBinding     *binding,
                  const GValue *from_value,
                  GValue       *to_value,
                  gpointer      user_data)
{
  ulong date = g_value_get_ulong (from_value);
  g_autofree char *tmp = NULL;

  tmp = stamp_time_helpers_utf_friendly_time (date, TRUE);
  g_value_set_string (to_value, tmp);

  return TRUE;
}

static gboolean
transfer_num_messages_to (GBinding     *binding,
                          const GValue *from_value,
                          GValue       *to_value,
                          gpointer      user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);
  ulong num = g_value_get_ulong (from_value);
  g_autofree char *tmp = NULL;

  tmp = g_strdup_printf ("%ld", num);
  g_value_set_string (to_value, tmp);

  /* Hint: We are using opacity here as otherwise we do have weird fast mouse scrolling zitter effects
   * (not scroll wheel) as widget will be recalculated every time. So let's leave it here and change
   * opacity...
   */
  if (num > 1)
    gtk_widget_set_opacity (GTK_WIDGET (self->counter), 1.0);
  else
    gtk_widget_set_opacity (GTK_WIDGET (self->counter), 0.0);

  return TRUE;
}

static gboolean
transfer_avatar_to (GBinding     *binding,
                    const GValue *from_value,
                    GValue       *to_value,
                    gpointer      user_data)
{
  const gchar *from = g_value_get_string (from_value);
  g_autofree char *tmp = NULL;

  tmp = stamp_strip_department (from);
  g_value_set_string (to_value, tmp);

  return TRUE;
}

void
stamp_conversation_row_unbind_mail (StampConversationRow  *self,
                                    StampConversationItem *item)
{
  g_return_if_fail (STAMP_IS_CONVERSATION_ROW (self));

  /* Cancel Photo-Loading */
  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  g_ptr_array_remove_range (self->bindings, 0, self->bindings->len);

  g_clear_object (&self->item);
}

static void
add_binding (StampConversationRow *self,
             GBinding             *binding)
{
  g_ptr_array_add (self->bindings, binding);
}

static gboolean
transfer_labels_to_box (GBinding     *binding,
                        const GValue *from,
                        GValue       *to,
                        gpointer      user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);
  GListStore *store = g_object_get_data (G_OBJECT (self->labels), "store");
  GPtrArray *labels = g_value_get_pointer (from);

  g_list_store_remove_all (store);

  if (labels) {
    for (gint idx = 0; idx < labels->len; idx++) {
      StampCategory *cat = stamp_account_find_category (self->account, labels->pdata[idx]);

      if (cat)
        g_list_store_append (store, cat);
    }
  }

  g_value_set_boolean (to, labels != NULL && labels->len != 0);

  return TRUE;
}

static GtkWidget *
create_label (gpointer item,
              gpointer user_data)
{
  StampCategory *obj = STAMP_CATEGORY (item);
  const gchar *name = stamp_category_get_name (obj);
  GtkWidget *label = gtk_label_new (name);
  g_autofree char *css_color = g_strdup_printf ("category-color-%s", stamp_category_get_color (obj));

  gtk_widget_add_css_class (label, "category-pill");
  gtk_widget_add_css_class (label, css_color);

  return label;
}

void
stamp_conversation_row_bind_mail (StampConversationRow  *self,
                                  StampConversationItem *item,
                                  StampAccount          *account)
{
  g_autoptr (GPtrArray) labels = NULL;
  g_autofree char *mail = NULL;
  StampPhotoToken *token;
  GListStore *store;

  stamp_conversation_row_unbind_mail (self, item);
  if (!item)
    return;

  self->item = g_object_ref (item);
  self->account = g_object_ref (account);

  self->generation++;
  self->cancellable = g_cancellable_new ();
  mail = stamp_conversation_item_get_mail (item);

  token = g_new0 (StampPhotoToken, 1);
  token->self = self;
  token->instance_id = self->instance_id;
  token->generation = self->generation;

  stamp_account_get_photo (account, mail, self->cancellable, on_get_photo, token);

  store = g_list_store_new (STAMP_TYPE_CATEGORY);
  gtk_flow_box_bind_model (self->labels, G_LIST_MODEL (store), create_label, NULL, NULL);
  g_object_set_data_full (G_OBJECT (self->labels), "store", store, g_object_unref);

  add_binding (self, g_object_bind_property (item, "subject", self->topic, "text", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property (item, "from", self->participants, "text", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property_full (item, "from", self->avatar, "text", G_BINDING_SYNC_CREATE, transfer_avatar_to, NULL, NULL, NULL));
  add_binding (self, g_object_bind_property (item, "unread", self, "unread", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property (item, "preview", self->body, "markup", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property_full (item, "flagged", self->flagged_icon, "icon-name", G_BINDING_SYNC_CREATE, transform_flagged_to, NULL, self, NULL));
  add_binding (self, g_object_bind_property (item, "has-attachment", self->attachment_icon, "visible", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property (item, "answered", self->reply_icon, "visible", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property (item, "forwarded", self->forwarded_icon, "visible", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property (item, "has-calendar", self->calendar_icon, "visible", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property (item, "important", self, "important", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property_full (item, "date", self->date, "label", G_BINDING_SYNC_CREATE, transfer_date_to, NULL, NULL, NULL));
  add_binding (self, g_object_bind_property_full (item, "num-messages", self->counter, "label", G_BINDING_SYNC_CREATE, transfer_num_messages_to, NULL, self, NULL));

  add_binding (self, g_object_bind_property_full (item, "labels", self->labels, "visible", G_BINDING_SYNC_CREATE, transfer_labels_to_box, NULL, self, NULL));
}

void
stamp_conversation_row_set_selection_visible (StampConversationRow *self,
                                              gboolean              visible)
{
  if (visible) {
    gtk_stack_set_visible_child (self->stack, GTK_WIDGET (self->check_button));
  } else {
    gtk_stack_set_visible_child (self->stack, GTK_WIDGET (self->avatar));
    gtk_check_button_set_active (self->check_button, FALSE);
  }
}

void
stamp_conversation_row_set_selection_active (StampConversationRow *self,
                                             gboolean              active)
{
  g_signal_handlers_block_by_func (self->check_button, on_check_button_toggled, self);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (self->check_button), active);
  g_signal_handlers_unblock_by_func (self->check_button, on_check_button_toggled, self);
}

gboolean
stamp_conversation_row_get_selection_active (StampConversationRow *self)
{
  return gtk_check_button_get_active (GTK_CHECK_BUTTON (self->check_button));
}

StampConversationItem *
stamp_conversation_row_get_item (StampConversationRow *self)
{
  return self->item;
}

GtkCheckButton *
stamp_conversation_row_get_check_button (StampConversationRow *self)
{
  return self->check_button;
}
