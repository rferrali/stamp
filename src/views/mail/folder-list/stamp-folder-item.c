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

#include "stamp-folder-item.h"

#include <glib/gi18n.h>
#include <gst/gst.h>

#include "stamp-account.h"
#include "stamp-item.h"
#include "stamp-settings.h"

struct _StampFolderItem {
  StampItem parent_instance;

  CamelFolderInfo *folder_info;
  CamelFolder *folder;
  GCancellable *cancellable;

  gint unread;
};

G_DEFINE_FINAL_TYPE (StampFolderItem, stamp_folder_item, STAMP_TYPE_ITEM);

typedef enum {
  PROP_FOLDER_INFO = 1,
  PROP_UNREAD,
} StampFolderItemProps;

static GParamSpec *properties[PROP_UNREAD + 1];

enum {
  FOLDER_ITEM_ADDED,
  LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = { 0 };

static const gchar *
get_icon (StampFolderItem *self,
          CamelFolderInfo *info)
{
  StampFolderType type = stamp_account_get_folder_order_rank (stamp_item_get_account (STAMP_ITEM (self)), info);

  switch (type) {
    case STAMP_FOLDER_TYPE_INBOX:
      return "inbox-symbolic";
    case STAMP_FOLDER_TYPE_STARRED:
      return "starred-symbolic";
    case STAMP_FOLDER_TYPE_IMPORTANT:
      return "mail-mark-important-symbolic";
    case STAMP_FOLDER_TYPE_OUTBOX:
      return "outbox-symbolic";
    case STAMP_FOLDER_TYPE_DRAFTS:
      return "drafts-symbolic";
    case STAMP_FOLDER_TYPE_SENT:
      return "sent-symbolic";
    case STAMP_FOLDER_TYPE_JUNK:
      return "junk-symbolic";
    case STAMP_FOLDER_TYPE_TRASH:
      return "user-trash-symbolic";
    case STAMP_FOLDER_TYPE_ARCHIVES:
      return "archive-symbolic";
    case STAMP_FOLDER_TYPE_TEMPLATES:
      return "folder-templates-symbolic";
    case STAMP_FOLDER_TYPE_NORMAL:
    default:
      break;
  }

  return "folder-symbolic";
}

static StampFolderItem *
stamp_folder_item_find_item (GListStore  *store,
                             const gchar *full_name)
{
  gint num = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (gint idx = 0; idx < num; idx++) {
    g_autoptr (StampFolderItem) item = g_list_model_get_item (G_LIST_MODEL (store), idx);
    GListStore *child_store;

    if (g_strcmp0 (stamp_folder_item_get_full_name (item), full_name) == 0)
      return g_steal_pointer (&item);

    child_store = stamp_item_get_list_store (STAMP_ITEM (item));
    if (child_store) {
      g_clear_object (&item);

      item = stamp_folder_item_find_item (child_store, full_name);
      if (item)
        return g_steal_pointer (&item);
    }
  }

  return NULL;
}

void
stamp_folder_item_set_folder_info (StampFolderItem *self,
                                   CamelFolderInfo *info)
{
  if (self->folder_info == info)
    return;

  if (self->folder_info)
    g_boxed_free (camel_folder_info_get_type (), self->folder_info);

  self->folder_info = g_boxed_copy (camel_folder_info_get_type (), info);

  stamp_item_set_name (STAMP_ITEM (self), self->folder_info->display_name);

  if (info->child) {
    CamelFolderInfo *current_folder_info = self->folder_info->child;
    GListStore *child_store = stamp_item_get_list_store (STAMP_ITEM (self));
    StampFolderItem *child_item;

    if (!child_store)
      return;

    child_item = stamp_folder_item_find_item (child_store, current_folder_info->full_name);
    while (current_folder_info) {
      stamp_folder_item_set_folder_info (child_item, current_folder_info);
      current_folder_info = current_folder_info->next;
    }
  }

  if (self->folder) {
    CamelFolderSummary *summary;

    summary = camel_folder_get_folder_summary (self->folder);
    self->unread = camel_folder_summary_get_unread_count (summary);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_UNREAD]);
  }
}

static void
stamp_folder_item_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  StampFolderItem *self = STAMP_FOLDER_ITEM (object);

  switch ((StampFolderItemProps)property_id) {
    case PROP_FOLDER_INFO:
      stamp_folder_item_set_folder_info (self, g_value_get_boxed (value));
      break;
    case PROP_UNREAD:
      break;
  }
}

static void
stamp_folder_item_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  StampFolderItem *self = STAMP_FOLDER_ITEM (object);

  switch ((StampFolderItemProps)property_id) {
    case PROP_FOLDER_INFO:
      g_value_set_boxed (value, self->folder_info);
      break;
    case PROP_UNREAD:
      g_value_set_uint (value, self->unread);
      break;
  }
}

static gint pending_new_mail_count = 0;
static gchar *pending_first_uid = NULL;
static gchar *pending_first_subject = NULL;
static gchar *pending_first_sender = NULL;
static guint pending_debounce_id = 0;

static void
play_incoming_sound (void)
{
  g_autoptr (GstElement) player = NULL;

  player = gst_element_factory_make ("playbin", NULL);
  g_object_set (player, "uri", "resource:///org/tabos/stamp/sounds/incoming.wav", NULL);
  gst_element_set_state (player, GST_STATE_PLAYING);
}

static void
send_pending_notification (gpointer user_data)
{
  GNotification *notification;
  g_autofree char *title = NULL;

  pending_debounce_id = 0;

  if (pending_new_mail_count == 0)
    return;

  if (pending_new_mail_count == 1) {
    title = g_strdup_printf ("%s", pending_first_sender ? pending_first_sender : _("New message"));
  } else {
    title = g_strdup_printf (ngettext ("%d new message", "%d new messages", pending_new_mail_count),
                             pending_new_mail_count);
  }

  notification = g_notification_new (title);

  if (pending_first_subject)
    g_notification_set_body (notification, pending_first_subject);

  if (pending_first_uid) {
    g_notification_add_button_with_target (notification, _("Show"), "app.show-message", "s", pending_first_uid);
    g_notification_set_default_action_and_target (notification, "app.show-message", "s", pending_first_uid);
  }

  g_application_send_notification (g_application_get_default (), "new-mail", notification);

  pending_new_mail_count = 0;
  g_clear_pointer (&pending_first_uid, g_free);
  g_clear_pointer (&pending_first_subject, g_free);
  g_clear_pointer (&pending_first_sender, g_free);
}

static void
on_folder_item_folder_changed (CamelFolder           *folder,
                               CamelFolderChangeInfo *changes,
                               gpointer               user_data)
{
  StampFolderItem *self = STAMP_FOLDER_ITEM (user_data);
  CamelFolderSummary *summary;
  CamelFolder *trash_folder = stamp_account_get_mail_trash_folder (stamp_item_get_account (STAMP_ITEM (self)));
  CamelFolder *draft_folder = stamp_account_get_mail_drafts_folder (stamp_item_get_account (STAMP_ITEM (self)));
  StampAccount *account = stamp_item_get_account (STAMP_ITEM (self));
  g_autofree char *settings_path = g_strconcat ("/org/tabos/stamp/mail/accounts/", stamp_account_get_name (account), "/", NULL);
  g_autoptr (GSettings) account_settings = g_settings_new_with_path ("org.tabos.stamp.mail.accounts", settings_path);
  g_autofree char *mode = g_settings_get_string (account_settings, "notification-mode");
  gboolean notify = TRUE;

  summary = camel_folder_get_folder_summary (folder);
  self->unread = camel_folder_summary_get_unread_count (summary);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_UNREAD]);

  if (!changes->uid_added || folder == trash_folder || folder == draft_folder)
    return;

  if (g_strcmp0 (mode, "inbox") == 0) {
    const gchar *full_name = camel_folder_get_full_name (folder);

    notify = (g_strcmp0 (full_name, "INBOX") == 0 || g_strcmp0 (full_name, "Inbox") == 0 || g_strcmp0 (full_name, "Posteingang") == 0 || g_str_has_prefix (full_name, "INBOX/"));
  } else if (g_strcmp0 (mode, "custom") == 0) {
    const gchar *full_name = camel_folder_get_full_name (folder);
    g_auto (GStrv) folders = g_settings_get_strv (account_settings, "notification-folders");

    notify = g_strv_contains ((const char **)folders, full_name);
  }

  if (!notify)
    return;

  for (gint idx = 0; idx < (int)changes->uid_added->len; idx++) {
    CamelMessageInfo *message_info;
    gchar *uid = (char *)changes->uid_added->pdata[idx];
    guint32 flags;

    message_info = camel_folder_summary_get (summary, uid);
    if (!message_info)
      continue;

    flags = camel_message_info_get_flags (message_info);
    if (flags & CAMEL_MESSAGE_SEEN)
      continue;

    pending_new_mail_count++;

    if (pending_new_mail_count == 1) {
      g_autoptr (CamelInternetAddress) address = camel_internet_address_new ();
      const gchar *sender_address = NULL;
      const gchar *sender_name = NULL;

      camel_address_unformat (CAMEL_ADDRESS (address), camel_message_info_get_from (message_info));
      camel_internet_address_get (address, 0, &sender_name, &sender_address);

      g_set_str (&pending_first_uid, camel_message_info_get_uid (message_info));
      g_set_str (&pending_first_subject, camel_message_info_get_subject (message_info));
      g_set_str (&pending_first_sender, sender_name ? sender_name : sender_address);
    }
  }

  if (pending_new_mail_count == 0)
    return;

  g_clear_handle_id (&pending_debounce_id, g_source_remove);

  pending_debounce_id = g_timeout_add_once (500, send_pending_notification, NULL);

  if (g_settings_get_boolean (STAMP_SETTINGS_MAIL, STAMP_PREFS_MAIL_PLAY_INCOMING_SOUND))
    play_incoming_sound ();
}

static void
on_get_folder (GObject      *source,
               GAsyncResult *res,
               gpointer      user_data)
{
  StampFolderItem *self = STAMP_FOLDER_ITEM (user_data);
  CamelStore *store = CAMEL_STORE (source);
  CamelFolderSummary *summary;
  g_autoptr (GError) error = NULL;

  stamp_item_set_loading (STAMP_ITEM (self), FALSE);
  self->folder = camel_store_get_folder_finish (store, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Could not load folder: %s", error->message);
    return;
  }

  g_signal_connect_object (self->folder, "changed", G_CALLBACK (on_folder_item_folder_changed), self, G_CONNECT_DEFAULT);

  summary = camel_folder_get_folder_summary (self->folder);
  self->unread = camel_folder_summary_get_unread_count (summary);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_UNREAD]);
}

static void
stamp_folder_item_constructed (GObject *object)
{
  StampFolderItem *self = STAMP_FOLDER_ITEM (object);
  StampAccount *account;
  g_autoptr (GError) error = NULL;
  StampMailService *mail_service;

  G_OBJECT_CLASS (stamp_folder_item_parent_class)->constructed (object);

  stamp_item_set_name (STAMP_ITEM (self), self->folder_info->display_name);
  stamp_item_set_icon_name (STAMP_ITEM (self), get_icon (self, self->folder_info));

  account = stamp_item_get_account (STAMP_ITEM (self));

  if (self->folder_info->child) {
    CamelFolderInfo *current_folder_info = self->folder_info->child;
    GListStore *list_store;

    g_debug ("%s: Adding children", G_STRFUNC);
    stamp_item_set_list_store_type (STAMP_ITEM (self), STAMP_TYPE_FOLDER_ITEM);
    list_store = stamp_item_get_list_store (STAMP_ITEM (self));

    while (current_folder_info) {
      g_autoptr (StampFolderItem) folder_item = stamp_folder_item_new (account, current_folder_info);
      g_list_store_append (list_store, STAMP_ITEM (folder_item));

      current_folder_info = current_folder_info->next;
    }
  }

  if (self->folder_info->flags & CAMEL_FOLDER_NOSELECT)
    return;

  if (self->folder_info->flags & CAMEL_FOLDER_VIRTUAL)
    return;

  if (self->folder_info->flags & CAMEL_FOLDER_VTRASH)
    return;

  mail_service = stamp_account_get_mail_service (account);

  stamp_item_set_loading (STAMP_ITEM (self), TRUE);
  g_debug ("%s: Loading folder %s", G_STRFUNC, self->folder_info->full_name);
  camel_store_get_folder (CAMEL_STORE (stamp_mail_service_get_service (mail_service)),
                          self->folder_info->full_name,
                          CAMEL_STORE_FOLDER_NONE,
                          G_PRIORITY_DEFAULT,
                          self->cancellable,
                          on_get_folder,
                          self);
}

static void
stamp_folder_item_dispose (GObject *object)
{
  StampFolderItem *self = STAMP_FOLDER_ITEM (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_signal_handlers_disconnect_by_func (self->folder,
                                        G_CALLBACK (on_folder_item_folder_changed),
                                        self);
  g_clear_object (&self->folder);

  g_boxed_free (camel_folder_info_get_type (), self->folder_info);

  G_OBJECT_CLASS (stamp_folder_item_parent_class)->dispose (object);
}

void
stamp_folder_item_class_init (StampFolderItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = stamp_folder_item_constructed;
  object_class->set_property = stamp_folder_item_set_property;
  object_class->get_property = stamp_folder_item_get_property;
  object_class->dispose = stamp_folder_item_dispose;

  properties[PROP_FOLDER_INFO] =
    g_param_spec_boxed ("folder-info",
                        NULL, NULL,
                        camel_folder_info_get_type (),
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  properties[PROP_UNREAD] =
    g_param_spec_uint ("unread",
                       NULL, NULL,
                       0,
                       G_MAXUINT,
                       0,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);

  signals[FOLDER_ITEM_ADDED] = g_signal_new ("folder-item-added", G_OBJECT_CLASS_TYPE (klass),
                                             G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                             0, NULL, NULL, NULL,
                                             G_TYPE_NONE,
                                             0);
}

void
stamp_folder_item_init (StampFolderItem *self)
{
}

StampFolderItem *
stamp_folder_item_new (StampAccount    *account,
                       CamelFolderInfo *folder_info)
{
  return g_object_new (STAMP_TYPE_FOLDER_ITEM,
                       "account", account,
                       "folder-info", folder_info,
                       NULL);
}

const gchar *
stamp_folder_item_get_full_name (StampFolderItem *self)
{
  return self->folder_info->full_name;
}

gint
stamp_folder_item_get_flags (StampFolderItem *self)
{
  return self->folder_info->flags;
}

void
stamp_folder_item_disconnect (StampFolderItem *self)
{
  GListStore *list_store = stamp_item_get_list_store (STAMP_ITEM (self));
  guint list_len;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  if (self->folder) {
    g_signal_handlers_disconnect_by_func (self->folder,
                                          G_CALLBACK (on_folder_item_folder_changed),
                                          self);
  }

  if (!list_store)
    return;

  list_len = g_list_model_get_n_items (G_LIST_MODEL (list_store));
  for (guint idx = 0; idx < list_len; idx++) {
    g_autoptr (StampFolderItem) item = g_list_model_get_item (G_LIST_MODEL (list_store), idx);

    stamp_folder_item_disconnect (item);
  }
}

CamelFolder *
stamp_folder_item_get_folder (StampFolderItem *self)
{
  return self->folder;
}

gint
stamp_folder_item_get_order_rank (StampFolderItem *self)
{
  return stamp_account_get_folder_order_rank (stamp_item_get_account (STAMP_ITEM (self)), self->folder_info);
}
