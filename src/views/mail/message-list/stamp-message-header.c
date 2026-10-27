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

#include "stamp-message-header.h"

#include <glib/gi18n.h>
#include <libsoup/soup.h>

#include "stamp-account.h"
#include "stamp-composer.h"
#include "stamp-helper.h"
#include "stamp-session.h"
#include "stamp-tag.h"
#include "stamp-time-helpers.h"
#include "stamp-window.h"

#define MAX_VISIBLE_TO  5
#define MAX_VISIBLE_CC  5

struct _StampMessageHeader {
  GtkGrid parent_instance;

  GtkWidget *avatar;
  GtkWidget *from_full;
  GtkWidget *attachment_icon;
  GtkWidget *starred_icon;
  GtkWidget *body_preview;
  GtkWidget *menu_button;
  GtkWidget *more_button;
  GtkWidget *cc_more_button;
  GtkWidget *to_box;
  GtkWidget *to;
  GtkWidget *to_wrap;
  GtkWidget *to_hidden_wrap;
  GtkWidget *cc_box;
  GtkWidget *cc;
  GtkWidget *cc_wrap;
  GtkWidget *cc_hidden_wrap;
  GtkWidget *date;
  GtkWidget *popover;
  GtkWidget *popover_avatar;
  GtkWidget *popover_name;
  GtkWidget *popover_email;
  GtkWidget *internal;
  GtkWidget *reply_icon;
  GtkWidget *forwarded_icon;

  gboolean to_filled;
  gboolean cc_filled;
  gboolean collapsed;
  gboolean is_unread;
  gboolean is_read;
  gboolean compact;
  guint total_to;
  guint total_cc;

  GCancellable *cancellable;
  GSimpleActionGroup *actions;
  const CamelMessageInfo *message_info;

  StampAccount *account;

  gchar *from_mail;
  gchar *from_name;
  gchar *sender_name;
  gchar *sender_mail;
};

G_DEFINE_FINAL_TYPE (StampMessageHeader, stamp_message_header, GTK_TYPE_GRID);

typedef enum {
  PROP_ACCOUNT = 1,
  PROP_COLLAPSED,
  PROP_COMPACT,
  PROP_IS_UNREAD,
  PROP_IS_READ,
} StampMessageHeaderProps;

static GParamSpec *properties[PROP_IS_READ + 1];

static void
update_visibility (StampMessageHeader *self);

static void
on_compose_activate (GSimpleAction *action,
                     GVariant      *param,
                     gpointer       user_data)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (user_data);
  g_autofree gchar *mail = g_strdup (gtk_label_get_text (GTK_LABEL (self->popover_email)));
  GtkWidget *composer = stamp_composer_new (self->account);

  stamp_composer_set_to (STAMP_COMPOSER (composer), mail);
  gtk_window_present (GTK_WINDOW (composer));
}

static void
on_copy_activate (GSimpleAction *action,
                  GVariant      *param,
                  gpointer       user_data)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (user_data);
  GdkDisplay *display;
  GdkClipboard *clipboard;

  display = gtk_widget_get_display (GTK_WIDGET (self));
  clipboard = gdk_display_get_clipboard (display);

  gdk_clipboard_set_text (clipboard, gtk_label_get_text (GTK_LABEL (self->popover_email)));
}

static void
on_search_activate (GSimpleAction *action,
                    GVariant      *param,
                    gpointer       user_data)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (user_data);
  GApplication *app = g_application_get_default ();
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  stamp_window_search_contact (STAMP_WINDOW (window), gtk_label_get_text (GTK_LABEL (self->popover_email)));
}

static void
on_show_contact_activate (GSimpleAction *action,
                          GVariant      *param,
                          gpointer       user_data)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (user_data);
  GApplication *app = g_application_get_default ();
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  stamp_window_show_contact (STAMP_WINDOW (window), gtk_label_get_text (GTK_LABEL (self->popover_email)));
}

static void
stamp_message_header_init (StampMessageHeader *self)
{
  g_autoptr (GSimpleAction) compose_action = g_simple_action_new ("compose", NULL);
  GSimpleAction *action = g_simple_action_new ("copy", NULL);
  GSimpleAction *search_action = g_simple_action_new ("search", NULL);
  GSimpleAction *show_contact_action = g_simple_action_new ("show-contact", NULL);

  self->actions = g_simple_action_group_new ();
  g_signal_connect (compose_action, "activate", G_CALLBACK (on_compose_activate), self);
  g_action_map_add_action (G_ACTION_MAP (self->actions), G_ACTION (compose_action));

  g_signal_connect (action, "activate", G_CALLBACK (on_copy_activate), self);
  g_action_map_add_action (G_ACTION_MAP (self->actions), G_ACTION (action));
  g_signal_connect (search_action, "activate", G_CALLBACK (on_search_activate), self);
  g_action_map_add_action (G_ACTION_MAP (self->actions), G_ACTION (search_action));
  g_signal_connect (show_contact_action, "activate", G_CALLBACK (on_show_contact_activate), self);
  g_action_map_add_action (G_ACTION_MAP (self->actions), G_ACTION (show_contact_action));

  gtk_widget_insert_action_group (GTK_WIDGET (self), "message-header", G_ACTION_GROUP (self->actions));

  gtk_widget_init_template (GTK_WIDGET (self));
  self->collapsed = TRUE;

  gtk_widget_set_cursor_from_name (self->more_button, "pointer");
  gtk_widget_set_cursor_from_name (self->cc_more_button, "pointer");

  gtk_widget_set_parent (self->popover, GTK_WIDGET (self->avatar));
}

static void
stamp_message_header_get_property (GObject    *object,
                                   guint       property_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (object);

  switch ((StampMessageHeaderProps)property_id) {
    case PROP_ACCOUNT:
      break;
    case PROP_COLLAPSED:
      g_value_set_boolean (value, self->collapsed);
      break;
    case PROP_COMPACT:
      g_value_set_boolean (value, self->compact);
      break;
    case PROP_IS_UNREAD:
      g_value_set_boolean (value, self->is_unread);
      break;
    case PROP_IS_READ:
      g_value_set_boolean (value, self->is_read);
      break;
  }
}

static void
stamp_message_header_set_property (GObject      *object,
                                   guint         property_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (object);

  switch ((StampMessageHeaderProps)property_id) {
    case PROP_ACCOUNT:
      self->account = g_value_get_object (value);
      break;
    case PROP_COLLAPSED:
      self->collapsed = g_value_get_boolean (value);
      update_visibility (self);
      break;
    case PROP_COMPACT:
      self->compact = g_value_get_boolean (value);
      adw_avatar_set_size (ADW_AVATAR (self->avatar), self->compact ? 24 : 48);
      update_visibility (self);
      break;
    case PROP_IS_UNREAD:
      self->is_unread = g_value_get_boolean (value);
      break;
    case PROP_IS_READ:
      self->is_read = g_value_get_boolean (value);
      break;
  }
}

static void
stamp_message_header_dispose (GObject *object)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_pointer (&self->popover, gtk_widget_unparent);

  G_OBJECT_CLASS (stamp_message_header_parent_class)->dispose (object);
}

static void
on_more_button_clicked (GtkWidget *button,
                        gpointer   user_data)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (user_data);
  guint hidden;
  g_autofree char *tmp_hidden = NULL;

  hidden = self->total_to - MAX_VISIBLE_TO;
  if (gtk_widget_get_visible (self->to_hidden_wrap)) {
    gtk_widget_set_visible (self->to_hidden_wrap, FALSE);
    tmp_hidden = g_strdup_printf ("+%u more", hidden);
  } else {
    gtk_widget_set_visible (self->to_hidden_wrap, TRUE);
    tmp_hidden = g_strdup_printf ("-%u less", hidden);
  }

  gtk_button_set_label (GTK_BUTTON (self->more_button), tmp_hidden);
}

static void
on_cc_more_button_clicked (GtkWidget *button,
                           gpointer   user_data)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (user_data);
  guint hidden;
  g_autofree char *tmp_hidden = NULL;

  hidden = self->total_cc - MAX_VISIBLE_CC;
  if (gtk_widget_get_visible (self->cc_hidden_wrap)) {
    gtk_widget_set_visible (self->cc_hidden_wrap, FALSE);
    tmp_hidden = g_strdup_printf ("+%u more", hidden);
  } else {
    gtk_widget_set_visible (self->cc_hidden_wrap, TRUE);
    tmp_hidden = g_strdup_printf ("-%u less", hidden);
  }

  gtk_button_set_label (GTK_BUTTON (self->cc_more_button), tmp_hidden);
}

static void
on_get_popover_photo (gpointer photo,
                      gpointer user_data)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (user_data);

  if (photo) {
    adw_avatar_set_custom_image (ADW_AVATAR (self->popover_avatar), GDK_PAINTABLE (photo));
  }
}

static CamelInternetAddress *
parse_custom_address (const gchar *str)
{
  g_autoptr (GRegex) regex = NULL;
  CamelInternetAddress *addr = camel_internet_address_new ();
  g_autoptr (GRegex) only_mail = g_regex_new ("^\\(([^)]+)\\)$", G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, NULL);
  g_autoptr (GMatchInfo) match = NULL;
  g_autofree char *tmp = g_strstrip (g_strdup (str));

  if (g_regex_match (only_mail, tmp, G_REGEX_MATCH_DEFAULT, &match)) {
    g_autofree char *email = g_match_info_fetch (match, 1);

    camel_internet_address_add (addr, email, email);
    return addr;
  }

  regex = g_regex_new ("^(\\S+(?:[,\\s]+\\S+)*)(?:\\s*\\(([^)]+)\\))?\\s*\\(([^)]+)\\)$", 0, 0, NULL);

  if (g_regex_match (regex, tmp, 0, &match)) {
    g_autofree char *name = g_match_info_fetch (match, 1);
    g_autofree char *email = g_match_info_fetch (match, 3);

    camel_internet_address_add (addr, name, email);
  } else {
    camel_address_decode (CAMEL_ADDRESS (addr), tmp);
  }

  return addr;
}

static void
on_released (GtkGesture *gesture,
             gint        n_press,
             gdouble     x,
             gdouble     y,
             gpointer    user_data)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (user_data);
  CamelInternetAddress *address = parse_custom_address (gtk_label_get_text (GTK_LABEL (self->from_full)));
  g_autofree char *tmp = NULL;
  g_autofree char *stripped = NULL;
  const gchar *name;
  const gchar *mail;

  camel_internet_address_get (address, 0, &name, &mail);

  tmp = g_markup_printf_escaped ("<b>%s</b>", name);
  stripped = stamp_strip_department (name);
  adw_avatar_set_text (ADW_AVATAR (self->popover_avatar), stripped);

  gtk_label_set_markup (GTK_LABEL (self->popover_name), tmp);
  gtk_label_set_text (GTK_LABEL (self->popover_email), mail);

  stamp_account_get_photo (self->account,
                           mail,
                           self->cancellable,
                           on_get_popover_photo,
                           self);

  gtk_popover_popup (GTK_POPOVER (self->popover));

  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
stamp_message_header_class_init (StampMessageHeaderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/message-list/stamp-message-header.ui");

  object_class->get_property = stamp_message_header_get_property;
  object_class->set_property = stamp_message_header_set_property;
  object_class->dispose = stamp_message_header_dispose;

  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, avatar);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, from_full);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, to_box);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, to);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, to_wrap);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, to_hidden_wrap);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, more_button);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, cc_box);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, cc);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, cc_wrap);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, cc_hidden_wrap);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, cc_more_button);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, date);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, attachment_icon);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, body_preview);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, menu_button);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, starred_icon);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, popover);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, popover_avatar);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, popover_name);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, popover_email);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, internal);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, reply_icon);
  gtk_widget_class_bind_template_child (widget_class, StampMessageHeader, forwarded_icon);

  gtk_widget_class_bind_template_callback (widget_class, on_more_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_cc_more_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_released);

  properties[PROP_ACCOUNT] =
    g_param_spec_object ("account",
                         NULL,
                         NULL,
                         STAMP_TYPE_ACCOUNT,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_COLLAPSED] =
    g_param_spec_boolean ("collapsed",
                          NULL,
                          NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_COMPACT] =
    g_param_spec_boolean ("compact",
                          NULL,
                          NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IS_UNREAD] =
    g_param_spec_boolean ("is-unread",
                          NULL,
                          NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IS_READ] =
    g_param_spec_boolean ("is-read",
                          NULL,
                          NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);
}

GtkWidget *
stamp_message_header_new (StampAccount *account)
{
  return g_object_new (STAMP_TYPE_MESSAGE_HEADER,
                       "account", account,
                       NULL);
}

static void
update_visibility (StampMessageHeader *self)
{
  /* Show when not collapsed */
  gtk_widget_set_visible (self->menu_button, !self->collapsed);
  gtk_widget_set_visible (self->to, !self->collapsed);
  gtk_widget_set_visible (self->to_wrap, !self->collapsed);
  gtk_widget_set_visible (self->more_button, !self->collapsed && self->total_to > MAX_VISIBLE_TO);
  gtk_widget_set_visible (self->cc, !self->collapsed);
  gtk_widget_set_visible (self->cc_wrap, !self->collapsed);
  gtk_widget_set_visible (self->cc_more_button, !self->collapsed && self->total_cc > MAX_VISIBLE_CC);

  /* Show when collapsed */
  gtk_widget_set_visible (self->body_preview, self->collapsed);
}

static void
on_get_photo (gpointer photo,
              gpointer user_data)
{
  g_autoptr (StampMessageHeader) self = STAMP_MESSAGE_HEADER (user_data);

  if (photo)
    adw_avatar_set_custom_image (ADW_AVATAR (self->avatar), GDK_PAINTABLE (photo));
}

static gboolean
has_thread_flag (CamelFolderThreadNode *node,
                 CamelMessageFlags      flag)
{
  if (!node)
    return FALSE;

  return (camel_message_info_get_flags (camel_folder_thread_node_get_item (node)) & flag) != 0;
}

static gboolean
has_attachment (CamelFolderThreadNode *thread_node)
{
  return has_thread_flag (thread_node, CAMEL_MESSAGE_ATTACHMENTS);
}

static gboolean
transfer_flags_to_icon (GBinding     *binding,
                        const GValue *from_value,
                        GValue       *to_value,
                        gpointer      user_data)
{
  StampMessageHeader *self = STAMP_MESSAGE_HEADER (user_data);
  guint flags = g_value_get_flags (from_value);

  if (flags & CAMEL_MESSAGE_FLAGGED)
    g_value_set_static_string (to_value, "starred-symbolic");
  else
    g_value_set_static_string (to_value, "non-starred-symbolic");

  gtk_widget_set_visible (self->reply_icon, flags & CAMEL_MESSAGE_ANSWERED);
  gtk_widget_set_visible (self->forwarded_icon, flags & CAMEL_MESSAGE_FORWARDED);

  return TRUE;
}

static void
update_from (StampMessageHeader *self)
{
  g_autofree char *tmp_name = NULL;
  g_autofree char *markup = NULL;

  if (!self->from_mail)
    return;

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  self->cancellable = g_cancellable_new ();

  if (self->account)
    stamp_account_get_photo (self->account,
                             self->from_mail,
                             self->cancellable,
                             on_get_photo,
                             g_object_ref (self));

  if (self->from_name && strlen (self->from_name) > 0) {
    g_autofree char *stripped_text = stamp_strip_department (self->from_name);

    adw_avatar_set_text (ADW_AVATAR (self->avatar), stripped_text);
  } else {
    adw_avatar_set_text (ADW_AVATAR (self->avatar), self->from_mail);
  }

  adw_avatar_set_show_initials (ADW_AVATAR (self->avatar), TRUE);

  if (self->sender_mail) {
    g_autofree char *tmp_sender_name = g_markup_escape_text (self->sender_name, -1);
    g_autofree char *tmp_sender_mail = g_markup_escape_text (self->sender_mail, -1);

    markup = g_strdup_printf (_("<b>%s</b> <small>(%s)</small>\non behalf of <small>%s (%s)</small>"),
                              tmp_sender_name,
                              tmp_sender_mail,
                              self->from_name,
                              self->from_mail);
  } else {
    g_autofree char *tmp_sender_name = g_markup_escape_text (self->from_name, -1);
    g_autofree char *tmp_sender_mail = g_markup_escape_text (self->from_mail, -1);

    markup = g_strdup_printf ("<b>%s</b> <small>(%s)</small>", tmp_sender_name, tmp_sender_mail);
  }

  gtk_label_set_markup (GTK_LABEL (self->from_full), markup);
  gtk_widget_set_tooltip_text (self->from_full, self->from_mail);
}

void
stamp_message_header_set_mail (StampMessageHeader    *self,
                               CamelFolderThreadNode *thread_node)
{
  g_autoptr (CamelInternetAddress) address = camel_internet_address_new ();
  const CamelMessageInfo *message_info = camel_folder_thread_node_get_item (thread_node);
  g_autofree char *markup = NULL;
  const gchar *to = NULL;
  const gchar *ia_name;
  const gchar *ia_address;
  g_autofree char *tmp = NULL;
  g_autoptr (GString) tmp_to = g_string_new (NULL);
  g_autoptr (GString) tmp_addresses = g_string_new (NULL);
  g_autoptr (GString) tmp_cc = g_string_new (NULL);
  g_autoptr (GString) tmp_cc_addresses = g_string_new (NULL);
  g_autoptr (GMenuItem) item = NULL;
  g_autofree char *time = NULL;
  guint hidden = 0;
  guint64 timestamp;

  self->message_info = message_info;

  if (camel_address_decode (CAMEL_ADDRESS (address), camel_message_info_get_from (message_info)) > 0) {
    camel_internet_address_get (address, 0, &ia_name, &ia_address);

    if (ia_name && strlen (ia_name) > 0) {
      g_autofree char *tmp_name = NULL;
      gint offset = 0;

      if (ia_name[0] == '<')
        offset = 1;

      tmp_name = g_strdup (ia_name + offset);
      if (tmp_name && tmp_name[strlen (tmp_name) - 1 ] == '>')
        tmp_name[strlen (tmp_name) - 1] = '\0';

      g_set_str (&self->from_name, tmp_name);
    } else {
      g_set_str (&self->from_name, ia_address);
    }

    g_set_str (&self->from_mail, ia_address);
  }

  gtk_inscription_set_text (GTK_INSCRIPTION (self->body_preview), camel_message_info_get_preview (message_info));
  update_from (self);

  g_clear_object (&address);
  address = camel_internet_address_new ();

  if (camel_address_decode (CAMEL_ADDRESS (address), camel_message_info_get_to (message_info)) > 0) {
    gint len = camel_address_length (CAMEL_ADDRESS (address));

    for (gint idx = 0; idx < len; idx++) {
      GtkWidget *tag = stamp_tag_new (self->account);

      camel_internet_address_get (address, idx, &ia_name, &ia_address);
      if (g_strcmp0 (ia_name, "") != 0) {
        to = ia_name;
      } else {
        to = ia_address;
      }

      stamp_tag_set_label (STAMP_TAG (tag), to);
      stamp_tag_set_mail (STAMP_TAG (tag), ia_address);
      stamp_tag_set_show_button (STAMP_TAG (tag), FALSE);

      self->total_to++;
      if (self->total_to < MAX_VISIBLE_TO) {
        adw_wrap_box_append (ADW_WRAP_BOX (self->to_wrap), tag);
      } else {
        adw_wrap_box_append (ADW_WRAP_BOX (self->to_hidden_wrap), tag);
      }
    }
  } else {
    gtk_widget_set_visible (self->to_box, FALSE);
  }

  hidden = self->total_to - MAX_VISIBLE_TO;
  if (hidden) {
    g_autofree char *tmp_hidden = NULL;

    tmp_hidden = g_strdup_printf ("+%u more", hidden);
    gtk_button_set_label (GTK_BUTTON (self->more_button), tmp_hidden);
    gtk_widget_set_visible (self->more_button, TRUE);
  }

  self->to_filled = TRUE;

  tmp = g_markup_printf_escaped ("<span alpha=\"55%%\">To:</span>");
  gtk_label_set_markup (GTK_LABEL (self->to), tmp);

  to = NULL;
  g_clear_object (&address);
  address = camel_internet_address_new ();

  if (camel_address_decode (CAMEL_ADDRESS (address), camel_message_info_get_cc (message_info)) > 0) {
    gint len = camel_address_length (CAMEL_ADDRESS (address));

    for (gint idx = 0; idx < len; idx++) {
      GtkWidget *tag = stamp_tag_new (self->account);

      camel_internet_address_get (address, idx, &ia_name, &ia_address);
      if (g_strcmp0 (ia_name, "") != 0) {
        to = ia_name;
      } else {
        to = ia_address;
      }

      stamp_tag_set_label (STAMP_TAG (tag), to);
      stamp_tag_set_mail (STAMP_TAG (tag), ia_address);
      stamp_tag_set_show_button (STAMP_TAG (tag), FALSE);

      self->total_cc++;
      if (self->total_cc < MAX_VISIBLE_CC) {
        adw_wrap_box_append (ADW_WRAP_BOX (self->cc_wrap), tag);
      } else {
        adw_wrap_box_append (ADW_WRAP_BOX (self->cc_hidden_wrap), tag);
      }
      self->cc_filled = TRUE;
    }

    g_clear_pointer (&tmp, g_free);
    tmp = g_markup_printf_escaped ("<span alpha=\"55%%\">Cc:</span>");
    gtk_label_set_markup (GTK_LABEL (self->cc), tmp);

    gtk_widget_set_visible (self->cc_box, TRUE);
  } else {
    gtk_widget_set_visible (self->cc_box, FALSE);
  }

  hidden = self->total_cc - MAX_VISIBLE_CC;
  if (hidden) {
    g_autofree char *tmp_hidden = NULL;

    tmp_hidden = g_strdup_printf ("+%u more", hidden);
    gtk_widget_set_visible (self->cc_more_button, TRUE);

    gtk_button_set_label (GTK_BUTTON (self->cc_more_button), tmp_hidden);
  }

  gtk_widget_set_visible (self->attachment_icon, has_attachment (thread_node));

  g_object_bind_property_full (G_OBJECT (message_info), "flags", self->starred_icon, "icon-name", G_BINDING_SYNC_CREATE, transfer_flags_to_icon, NULL, self, NULL);

  timestamp = camel_message_info_get_date_received (message_info);
  if (!timestamp)
    timestamp = camel_message_info_get_date_sent (message_info);

  time = stamp_time_helpers_utf_friendly_time (timestamp, FALSE);
  gtk_label_set_text (GTK_LABEL (self->date), time);

  update_visibility (self);
}

void
stamp_message_header_set_collapsed (StampMessageHeader *self,
                                    gboolean            collapsed)
{
  self->collapsed = !collapsed;
  update_visibility (self);
}

void
stamp_message_header_set_account (StampMessageHeader *self,
                                  StampAccount       *account)
{
  self->account = account;
}

void
stamp_message_header_set_internal (StampMessageHeader *self,
                                   gboolean            is_internal)
{
  gtk_widget_set_visible (self->internal, is_internal);
}

void
stamp_message_header_set_sender (StampMessageHeader *self,
                                 const gchar        *sender)
{
  g_autoptr (CamelInternetAddress) address = camel_internet_address_new ();
  const gchar *ia_name;
  const gchar *ia_address;

  if (camel_address_decode (CAMEL_ADDRESS (address), sender) <= 0)
    return;

  camel_internet_address_get (address, 0, &ia_name, &ia_address);
  g_set_str (&self->sender_name, ia_name);
  g_set_str (&self->sender_mail, ia_address);

  update_from (self);
}
