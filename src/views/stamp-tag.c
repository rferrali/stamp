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

#include "stamp-tag.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "stamp-account.h"
#include "stamp-composer.h"
#include "stamp-helper.h"
#include "stamp-session.h"
#include "stamp-window.h"

struct _StampTag {
  GtkBox parent_instance;

  GtkLabel *label;
  GtkButton *button;
  GtkPopoverMenu *popover;
  AdwAvatar *popover_avatar;
  GtkLabel *popover_name;
  GtkLabel *popover_email;

  gchar *mail;
  GCancellable *cancellable;
  StampAccount *account;
};

G_DEFINE_FINAL_TYPE (StampTag, stamp_tag, GTK_TYPE_BOX);

typedef enum {
  PROP_LABEL = 1,
  PROP_ACCOUNT,
} StampTagProps;

static GParamSpec *obj_properties[PROP_ACCOUNT + 1];

static void
stamp_tag_get_property (GObject    *object,
                        guint       property_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  StampTag *self = STAMP_TAG (object);

  switch ((StampTagProps)property_id) {
    case PROP_ACCOUNT:
      g_value_set_object (value, self->account);
      break;
    case PROP_LABEL:
      break;
  }
}

static void
on_get_photo (gpointer photo,
              gpointer user_data)
{
  StampTag *self = STAMP_TAG (user_data);

  if (photo)
    adw_avatar_set_custom_image (self->popover_avatar, GDK_PAINTABLE (photo));
}

static void
stamp_tag_set_property (GObject      *object,
                        guint         property_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  StampTag *self = STAMP_TAG (object);

  switch ((StampTagProps)property_id) {
    case PROP_ACCOUNT:
      g_clear_object (&self->account);

      self->account = g_value_get_object (value);
      if (self->account)
        g_object_ref (self->account);

      break;
    case PROP_LABEL:
      gtk_label_set_text (self->label, g_value_get_string (value));
      break;
  }
}

static void
on_released (GtkGesture *gesture,
             gint        n_press,
             gdouble     x,
             gdouble     y,
             gpointer    user_data)
{
  StampTag *self = STAMP_TAG (user_data);
  const gchar *label = gtk_label_get_text (self->label);
  g_autofree char *stripped = stamp_strip_department (label);
  g_autofree char *tmp = g_markup_printf_escaped ("<b>%s</b>", label);

  gtk_label_set_markup (self->popover_name, tmp);
  adw_avatar_set_text (self->popover_avatar, stripped);

  stamp_account_get_photo (self->account, self->mail, self->cancellable, on_get_photo, self);

  gtk_popover_popup (GTK_POPOVER (self->popover));
  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_compose_activate (GSimpleAction *action,
                     GVariant      *param,
                     gpointer       user_data)
{
  StampTag *self = STAMP_TAG (user_data);
  GtkWidget *composer = stamp_composer_new (self->account);

  stamp_composer_set_to (STAMP_COMPOSER (composer), self->mail);
  gtk_window_present (GTK_WINDOW (composer));
}

static void
on_copy_activate (GSimpleAction *action,
                  GVariant      *param,
                  gpointer       user_data)
{
  StampTag *self = STAMP_TAG (user_data);
  GdkDisplay *display;
  GdkClipboard *clipboard;

  display = gtk_widget_get_display (GTK_WIDGET (self));
  clipboard = gdk_display_get_clipboard (display);

  gdk_clipboard_set_text (clipboard, stamp_tag_get_mail (self));
}

static void
on_search_activate (GSimpleAction *action,
                    GVariant      *param,
                    gpointer       user_data)
{
  StampTag *self = STAMP_TAG (user_data);
  GApplication *app = g_application_get_default ();
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  stamp_window_search_contact (STAMP_WINDOW (window), self->mail);
}

static void
on_show_contact_activate (GSimpleAction *action,
                          GVariant      *param,
                          gpointer       user_data)
{
  StampTag *self = STAMP_TAG (user_data);
  GApplication *app = g_application_get_default ();
  StampWindow *window = STAMP_WINDOW (gtk_application_get_active_window (GTK_APPLICATION (app)));

  stamp_window_show_contact (window, self->mail);
}

static void
stamp_tag_init (StampTag *self)
{
  GSimpleActionGroup *group = g_simple_action_group_new ();
  g_autoptr (GSimpleAction) compose_action = g_simple_action_new ("compose", NULL);
  GSimpleAction *action = g_simple_action_new ("copy", NULL);
  GSimpleAction *search_action = g_simple_action_new ("search", NULL);
  GSimpleAction *show_contact_action = g_simple_action_new ("show-contact", NULL);

  g_signal_connect (compose_action, "activate", G_CALLBACK (on_compose_activate), self);
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (compose_action));

  g_signal_connect (action, "activate", G_CALLBACK (on_copy_activate), self);
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (action));

  g_signal_connect (search_action, "activate", G_CALLBACK (on_search_activate), self);
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (search_action));

  g_signal_connect (show_contact_action, "activate", G_CALLBACK (on_show_contact_activate), self);
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (show_contact_action));

  gtk_widget_insert_action_group (GTK_WIDGET (self), "tag", G_ACTION_GROUP (group));

  gtk_widget_init_template (GTK_WIDGET (self));

  self->cancellable = g_cancellable_new ();

  gtk_widget_set_parent (GTK_WIDGET (self->popover), GTK_WIDGET (self));
}

static void
on_button_clicked (GtkWidget *button,
                   gpointer   user_data)
{
  StampTag *self = STAMP_TAG (user_data);

  gtk_widget_unparent (GTK_WIDGET (self));
}

static void
stamp_tag_dispose (GObject *object)
{
  StampTag *self = STAMP_TAG (object);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  g_clear_object (&self->cancellable);

  g_clear_pointer (&self->mail, g_free);
  g_clear_object (&self->account);

  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_TAG);

  G_OBJECT_CLASS (stamp_tag_parent_class)->dispose (object);
}

static void
stamp_tag_class_init (StampTagClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/stamp-tag.ui");

  object_class->get_property = stamp_tag_get_property;
  object_class->set_property = stamp_tag_set_property;
  object_class->dispose = stamp_tag_dispose;

  gtk_widget_class_bind_template_child (widget_class, StampTag, label);
  gtk_widget_class_bind_template_child (widget_class, StampTag, button);
  gtk_widget_class_bind_template_child (widget_class, StampTag, popover);
  gtk_widget_class_bind_template_child (widget_class, StampTag, popover_avatar);
  gtk_widget_class_bind_template_child (widget_class, StampTag, popover_name);
  gtk_widget_class_bind_template_child (widget_class, StampTag, popover_email);

  gtk_widget_class_bind_template_callback (widget_class, on_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_released);

  obj_properties[PROP_LABEL] = g_param_spec_string ("label",
                                                    NULL,
                                                    NULL,
                                                    "",
                                                    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  obj_properties[PROP_ACCOUNT] = g_param_spec_object ("account",
                                                      NULL,
                                                      NULL,
                                                      STAMP_TYPE_ACCOUNT,
                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (obj_properties), obj_properties);
}

GtkWidget *
stamp_tag_new (StampAccount *account)
{
  return g_object_new (STAMP_TYPE_TAG, "account", account, NULL);
}

void
stamp_tag_set_label (StampTag    *self,
                     const gchar *label)
{
  gtk_label_set_text (self->label, label);
}

void
stamp_tag_set_mail (StampTag    *self,
                    const gchar *mail)
{
  g_set_str (&self->mail, mail);

  gtk_label_set_text (self->popover_email, mail);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self), mail);
}

const gchar *
stamp_tag_get_mail (StampTag *self)
{
  return self->mail;
}

const gchar *
stamp_tag_get_label (StampTag *self)
{
  return gtk_label_get_text (self->label);
}

void
stamp_tag_set_show_button (StampTag *self,
                           gboolean  show)
{
  gtk_widget_set_visible (GTK_WIDGET (self->button), show);
}
