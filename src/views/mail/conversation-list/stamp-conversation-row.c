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
#include "stamp-settings.h"
#include "stamp-time-helpers.h"

typedef enum {
  SWIPE_ACTION_NONE,
  SWIPE_ACTION_TOGGLE_READ,
  SWIPE_ACTION_TRASH,
} SwipeAction;

#define SWIPE_DISTANCE 96
#define SWIPE_THRESHOLD 90

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
  GtkScrolledWindow *left_action;
  GtkLabel *left_action_label;
  GtkImage *left_action_image;
  GtkScrolledWindow *right_action;
  GtkLabel *right_action_label;
  GtkImage *right_action_image;
  GtkGrid *row;
  GtkImage *attachment_icon;
  GtkImage *reply_icon;
  GtkImage *forwarded_icon;
  GtkImage *calendar_icon;
  GtkFlowBox *labels;
  GtkLabel *account_pill;
  const gchar *account_color_class;

  GCancellable *cancellable;
  gboolean selected;
  gboolean claimed;
  AdwAnimation *swipe_animation;
  gdouble swipe_offset;
  gboolean scrolling;
  gdouble scroll_offset;
  StampConversationItem *item;
  GPtrArray *bindings;
  guint instance_id;
  guint generation;
  gboolean unread;
  SwipeAction swipe_right;
  SwipeAction swipe_left;

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
set_swipe_offset (gdouble  offset,
                  gpointer user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);
  GtkWidget *left = GTK_WIDGET (self->left_action);
  GtkWidget *right = GTK_WIDGET (self->right_action);
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));
  gboolean left_ready = offset > SWIPE_THRESHOLD;
  gboolean right_ready = offset < -SWIPE_THRESHOLD;

  self->swipe_offset = offset;
  gtk_widget_set_margin_start (GTK_WIDGET (self->row), offset);
  gtk_widget_set_margin_end (GTK_WIDGET (self->row), -offset);

  gtk_widget_set_size_request (left, MAX (1, (gint)offset), -1);
  gtk_widget_set_size_request (right, MAX (1, (gint) - offset), -1);
  gtk_widget_set_visible (left, offset > 0);
  gtk_widget_set_visible (right, offset < 0);
  gtk_widget_set_opacity (left, CLAMP (offset / 32.0, 0, 1));
  gtk_widget_set_opacity (right, CLAMP (-offset / 32.0, 0, 1));

  gtk_widget_set_opacity (GTK_WIDGET (self->left_action_label), CLAMP ((offset - 64) / 24.0, 0, 1));
  gtk_widget_set_opacity (GTK_WIDGET (self->right_action_label), CLAMP ((-offset - 64) / 24.0, 0, 1));

  if (parent && g_strcmp0 (gtk_widget_get_css_name (parent), "row") == 0) {
    if (offset != 0)
      gtk_widget_add_css_class (parent, "swipe-active");
    else
      gtk_widget_remove_css_class (parent, "swipe-active");
  }

  if (left_ready)
    gtk_widget_add_css_class (left, "ready");
  else
    gtk_widget_remove_css_class (left, "ready");

  if (right_ready)
    gtk_widget_add_css_class (right, "ready");
  else
    gtk_widget_remove_css_class (right, "ready");
}

static void
reset_swipe (StampConversationRow *self)
{
  if (self->swipe_animation) {
    adw_animation_pause (self->swipe_animation);
    g_clear_object (&self->swipe_animation);
  }

  self->claimed = FALSE;
  set_swipe_offset (0, self);
}

static void
stamp_conversation_row_dispose (GObject *object)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (object);

  reset_swipe (self);

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

static SwipeAction
get_swipe_action (const gchar *key)
{
  g_autofree gchar *name = g_settings_get_string (STAMP_SETTINGS_MAIL, key);

  if (g_strcmp0 (name, "toggle-read") == 0)
    return SWIPE_ACTION_TOGGLE_READ;
  if (g_strcmp0 (name, "trash") == 0)
    return SWIPE_ACTION_TRASH;

  return SWIPE_ACTION_NONE;
}

static void
set_action_label (GtkLabel    *label,
                  const gchar *text)
{
  gtk_label_set_text (label, text);
  gtk_accessible_update_property (GTK_ACCESSIBLE (label), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
}

static void
update_action_widget (StampConversationRow *self,
                      GtkWidget            *box,
                      GtkLabel             *label,
                      GtkImage             *image,
                      SwipeAction           action)
{
  gtk_widget_remove_css_class (box, "accent");
  gtk_widget_remove_css_class (box, "error");

  switch (action) {
    case SWIPE_ACTION_TOGGLE_READ:
      gtk_widget_add_css_class (box, "accent");
      set_action_label (label, self->unread ? _("Mark Read") : _("Mark Unread"));
      gtk_image_set_from_icon_name (image, self->unread ? "mail-read-symbolic" : "mail-unread-symbolic");
      break;
    case SWIPE_ACTION_TRASH:
      gtk_widget_add_css_class (box, "error");
      set_action_label (label, _("Trash"));
      gtk_image_set_from_icon_name (image, "user-trash-symbolic");
      break;
    case SWIPE_ACTION_NONE:
      break;
  }
}

static void
update_swipe_actions (StampConversationRow *self)
{
  gint left_height = 0;
  gint right_height = 0;

  self->swipe_right = get_swipe_action (STAMP_PREFS_MAIL_SWIPE_RIGHT_ACTION);
  self->swipe_left = get_swipe_action (STAMP_PREFS_MAIL_SWIPE_LEFT_ACTION);

  update_action_widget (self, GTK_WIDGET (self->left_action), self->left_action_label, self->left_action_image, self->swipe_right);
  update_action_widget (self, GTK_WIDGET (self->right_action), self->right_action_label, self->right_action_image, self->swipe_left);

  if (self->swipe_right != SWIPE_ACTION_NONE)
    gtk_widget_measure (gtk_scrolled_window_get_child (self->left_action), GTK_ORIENTATION_VERTICAL, SWIPE_DISTANCE, NULL, &left_height, NULL, NULL);

  if (self->swipe_left != SWIPE_ACTION_NONE)
    gtk_widget_measure (gtk_scrolled_window_get_child (self->right_action), GTK_ORIENTATION_VERTICAL, SWIPE_DISTANCE, NULL, &right_height, NULL, NULL);

  gtk_widget_set_size_request (GTK_WIDGET (self->row), -1, MAX (left_height, right_height) + 16);
}

static void
on_swipe_settings_changed (GSettings *settings,
                           gchar     *key,
                           gpointer   user_data)
{
  update_swipe_actions (STAMP_CONVERSATION_ROW (user_data));
}

static void
trigger_swipe_action (StampConversationRow *self,
                      SwipeAction           action)
{
  switch (action) {
    case SWIPE_ACTION_TOGGLE_READ:
      g_signal_emit (self, self->unread ? signals[MARK_READ] : signals[MARK_UNREAD], 0);
      break;
    case SWIPE_ACTION_TRASH:
      g_signal_emit (self, signals[TRASH], 0);
      break;
    case SWIPE_ACTION_NONE:
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
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self->topic), "caption");
    gtk_widget_remove_css_class (GTK_WIDGET (self->body), "dim-label");
    gtk_widget_add_css_class (GTK_WIDGET (self->topic), "caption-heading");
    gtk_widget_add_css_class (GTK_WIDGET (self->participants), "heading");
    gtk_widget_add_css_class (GTK_WIDGET (self->topic), "accent");
  }

  update_swipe_actions (self);
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

static gdouble
clamp_swipe_offset (StampConversationRow *self,
                    gdouble               offset)
{
  return CLAMP (offset,
                self->swipe_left == SWIPE_ACTION_NONE ? 0 : -SWIPE_DISTANCE,
                self->swipe_right == SWIPE_ACTION_NONE ? 0 : SWIPE_DISTANCE);
}

static void
on_drag_update (GtkGestureDrag *gesture,
                gdouble         dx,
                gdouble         dy,
                gpointer        user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);

  if (!self->claimed) {
    if (fabs (dx) < 10 || fabs (dx) <= fabs (dy))
      return;

    if (!gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED))
      return;

    reset_swipe (self);
    self->claimed = TRUE;
  }

  set_swipe_offset (clamp_swipe_offset (self, dx), self);
}

static void
animate_to_zero (StampConversationRow *self)
{
  AdwAnimationTarget *target = adw_callback_animation_target_new (set_swipe_offset, self, NULL);

  if (self->swipe_animation)
    adw_animation_pause (self->swipe_animation);

  g_clear_object (&self->swipe_animation);

  self->swipe_animation = adw_timed_animation_new (GTK_WIDGET (self->row), self->swipe_offset, 0, 200, target);
  adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->swipe_animation), ADW_EASE_OUT_CUBIC);
  adw_animation_play (self->swipe_animation);
}

static void
on_drag_end (GtkGestureDrag *gesture,
             gdouble         dx,
             gdouble         dy,
             gpointer        user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);
  gdouble pos;
  SwipeAction action = SWIPE_ACTION_NONE;

  if (!self->claimed)
    return;

  pos = clamp_swipe_offset (self, dx);
  self->claimed = FALSE;

  if (pos > SWIPE_THRESHOLD)
    action = self->swipe_right;
  else if (pos < -SWIPE_THRESHOLD)
    action = self->swipe_left;

  animate_to_zero (self);
  trigger_swipe_action (self, action);
}

static void
on_drag_cancel (GtkGesture       *gesture,
                GdkEventSequence *sequence,
                gpointer          user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);

  if (!self->claimed)
    return;

  self->claimed = FALSE;
  animate_to_zero (self);
}

static void
on_swipe_scroll_begin (GtkEventControllerScroll *controller,
                       gpointer                  user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);

  self->scrolling = FALSE;
  self->scroll_offset = 0;
}

static gboolean
on_swipe_scroll (GtkEventControllerScroll *controller,
                 gdouble                   dx,
                 gdouble                   dy,
                 gpointer                  user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);
  GdkEvent *event;

  if (self->swipe_left == SWIPE_ACTION_NONE && self->swipe_right == SWIPE_ACTION_NONE)
    return GDK_EVENT_PROPAGATE;

  event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller));
  if (!event || !GDK_IS_EVENT_TYPE (event, GDK_SCROLL) || gdk_scroll_event_get_direction (event) != GDK_SCROLL_SMOOTH)
    return GDK_EVENT_PROPAGATE;

  if (!self->scrolling) {
    if (fabs (dx) < 1 || fabs (dx) <= fabs (dy))
      return GDK_EVENT_PROPAGATE;

    reset_swipe (self);
    self->scrolling = TRUE;
  }

  self->scroll_offset -= dx;
  set_swipe_offset (clamp_swipe_offset (self, self->scroll_offset), self);

  return GDK_EVENT_STOP;
}

static void
on_swipe_scroll_end (GtkEventControllerScroll *controller,
                     gpointer                  user_data)
{
  StampConversationRow *self = STAMP_CONVERSATION_ROW (user_data);
  gdouble pos;
  SwipeAction action = SWIPE_ACTION_NONE;

  if (!self->scrolling)
    return;

  pos = clamp_swipe_offset (self, self->scroll_offset);

  self->scrolling = FALSE;
  self->scroll_offset = 0;

  if (pos > SWIPE_THRESHOLD)
    action = self->swipe_right;
  else if (pos < -SWIPE_THRESHOLD)
    action = self->swipe_left;

  animate_to_zero (self);
  trigger_swipe_action (self, action);
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
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, right_action_label);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, right_action_image);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, row);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, attachment_icon);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, reply_icon);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, forwarded_icon);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, calendar_icon);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, labels);
  gtk_widget_class_bind_template_child (widget_class, StampConversationRow, account_pill);

  gtk_widget_class_bind_template_callback (widget_class, on_drag_update);
  gtk_widget_class_bind_template_callback (widget_class, on_drag_end);
  gtk_widget_class_bind_template_callback (widget_class, on_drag_cancel);
  gtk_widget_class_bind_template_callback (widget_class, on_swipe_scroll_begin);
  gtk_widget_class_bind_template_callback (widget_class, on_swipe_scroll);
  gtk_widget_class_bind_template_callback (widget_class, on_swipe_scroll_end);
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

  update_swipe_actions (self);
  g_signal_connect_object (STAMP_SETTINGS_MAIL, "changed::" STAMP_PREFS_MAIL_SWIPE_RIGHT_ACTION, G_CALLBACK (on_swipe_settings_changed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (STAMP_SETTINGS_MAIL, "changed::" STAMP_PREFS_MAIL_SWIPE_LEFT_ACTION, G_CALLBACK (on_swipe_settings_changed), self, G_CONNECT_DEFAULT);
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

static void
set_date (StampConversationRow *self,
          gulong                date)
{
  g_autofree char *tmp = stamp_time_helpers_utf_friendly_time (date, TRUE);

  gtk_label_set_label (self->date, tmp);
}

static void
set_num_messages (StampConversationRow *self,
                  gulong                num)
{
  g_autofree char *tmp = g_strdup_printf ("%ld", num);

  gtk_label_set_label (self->counter, tmp);

  /* Hint: We are using opacity here as otherwise we do have weird fast mouse scrolling zitter effects
   * (not scroll wheel) as widget will be recalculated every time. So let's leave it here and change
   * opacity...
   */
  gtk_widget_set_opacity (GTK_WIDGET (self->counter), num > 1 ? 1.0 : 0.0);
}

static void
set_avatar (StampConversationRow *self,
            const gchar          *from)
{
  g_autofree char *tmp = stamp_strip_department (from);

  adw_avatar_set_text (self->avatar, tmp);
}

static void
on_account_color_changed (StampAccount         *account,
                          StampConversationRow *self)
{
  if (self->account_color_class)
    gtk_widget_remove_css_class (GTK_WIDGET (self->account_pill), self->account_color_class);

  self->account_color_class = stamp_account_get_color_class (account);

  if (self->account_color_class)
    gtk_widget_add_css_class (GTK_WIDGET (self->account_pill), self->account_color_class);
}

void
stamp_conversation_row_unbind_mail (StampConversationRow  *self,
                                    StampConversationItem *item)
{
  g_return_if_fail (STAMP_IS_CONVERSATION_ROW (self));

  reset_swipe (self);

  /* Cancel Photo-Loading */
  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  adw_avatar_set_custom_image (self->avatar, NULL);

  if (self->account_color_class) {
    gtk_widget_remove_css_class (GTK_WIDGET (self->account_pill), self->account_color_class);
    self->account_color_class = NULL;
  }

  if (self->account) {
    g_signal_handlers_disconnect_by_func (self->account, on_account_color_changed, self);
    g_clear_object (&self->account);
  }

  gtk_label_set_text (GTK_LABEL (self->account_pill), NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->account_pill), FALSE);

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
  const gchar *color = stamp_category_get_color (obj);
  GdkDisplay *display = gtk_widget_get_display (label);
  g_autofree char *css_class = NULL;

  if (!color)
    color = "#808080";

  css_class = g_strdup_printf ("category-color-%s", color + 1);

  if (!g_object_get_data (G_OBJECT (display), css_class)) {
    g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
    g_autofree char *css = g_strdup_printf (".%s { background-color: mix(%s, white, 0.5); border-color: %s; color: #1a1a1a; }", css_class, color, color);

    gtk_css_provider_load_from_string (provider, css);
    gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set_data (G_OBJECT (display), css_class, GINT_TO_POINTER (1));
  }

  gtk_widget_add_css_class (label, "category-pill");
  gtk_widget_add_css_class (label, css_class);

  return label;
}

void
stamp_conversation_row_bind_mail (StampConversationRow  *self,
                                  StampConversationItem *item,
                                  StampAccount          *account,
                                  gboolean               show_account)
{
  g_autoptr (GPtrArray) labels = NULL;
  g_autofree char *mail = NULL;
  StampPhotoToken *token;
  GListStore *store;

  if (!item)
    return;

  /* The list view always unbinds before rebinding a row */
  g_return_if_fail (self->item == NULL);

  self->item = g_object_ref (item);
  if (account)
    self->account = g_object_ref (account);

  if (show_account && account) {
    self->account_color_class = stamp_account_get_color_class (account);
    if (self->account_color_class)
      gtk_widget_add_css_class (GTK_WIDGET (self->account_pill), self->account_color_class);

    gtk_label_set_text (GTK_LABEL (self->account_pill), stamp_account_get_name (account));
    gtk_widget_set_visible (GTK_WIDGET (self->account_pill), TRUE);

    g_signal_connect_object (account, "color-changed",
                             G_CALLBACK (on_account_color_changed), self,
                             G_CONNECT_DEFAULT);
  }

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

  /* Only properties which can change while bound need a live binding, everything else is set once */
  gtk_inscription_set_text (self->topic, stamp_conversation_item_get_subject (item));
  gtk_inscription_set_text (self->participants, stamp_conversation_item_get_from (item));
  set_avatar (self, stamp_conversation_item_get_from (item));
  gtk_inscription_set_text (self->body, stamp_conversation_item_get_preview (item));
  gtk_widget_set_visible (GTK_WIDGET (self->attachment_icon), stamp_conversation_item_has_attachment (item));
  gtk_widget_set_visible (GTK_WIDGET (self->calendar_icon), stamp_conversation_item_get_calendar (item));
  g_object_set (self, "important", stamp_conversation_item_is_important (item), NULL);
  set_date (self, stamp_conversation_item_get_date (item));
  set_num_messages (self, stamp_conversation_item_get_num_messages (item));

  add_binding (self, g_object_bind_property (item, "unread", self, "unread", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property_full (item, "flagged", self->flagged_icon, "icon-name", G_BINDING_SYNC_CREATE, transform_flagged_to, NULL, self, NULL));
  add_binding (self, g_object_bind_property (item, "answered", self->reply_icon, "visible", G_BINDING_SYNC_CREATE));
  add_binding (self, g_object_bind_property (item, "forwarded", self->forwarded_icon, "visible", G_BINDING_SYNC_CREATE));
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
