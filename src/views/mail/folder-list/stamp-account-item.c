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

#include "stamp-account-item.h"

#include <camel/camel.h>
#include <glib/gi18n.h>

#include "stamp-account.h"
#include "stamp-folder-item.h"
#include "stamp-item.h"
#include "stamp-session.h"
#include "stamp-settings.h"

struct _StampAccountItem {
  StampItem parent_instance;

  GCancellable *cancellable;
  CamelOfflineStore *offline_store;
  GSettings *refresh_settings;
  guint refresh_timer_id;

  GQueue *refresh_queue;
  gboolean refresh_queue_running;
  gboolean first_refresh;
  guint refresh_folder_handler;
};

G_DEFINE_FINAL_TYPE (StampAccountItem, stamp_account_item, STAMP_TYPE_ITEM);

static void
stamp_account_item_connect_to_account (StampAccountItem *self);

enum {
  ACCOUNT_ITEM_CHANGED,
  LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = { 0 };

static StampFolderItem *
stamp_account_item_find_item (GListStore  *store,
                              const gchar *full_name)
{
  guint list_len = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (guint idx = 0; idx < list_len; idx++) {
    StampFolderItem *item = g_list_model_get_item (G_LIST_MODEL (store), idx);
    GListStore *child_store;

    if (g_strcmp0 (stamp_folder_item_get_full_name (item), full_name) == 0) {
      return item;
    }

    child_store = stamp_item_get_list_store (STAMP_ITEM (item));
    if (child_store) {
      item = stamp_account_item_find_item (child_store, full_name);
      if (item)
        return item;
    }
  }
  return NULL;
}

static void
on_folder_item_changed (GtkWidget *folder,
                        gpointer   user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);

  g_signal_emit (self, signals[ACCOUNT_ITEM_CHANGED], 0);
}

static void
add_folder_info_item (StampAccountItem *self,
                      GPtrArray        *items,
                      CamelFolderInfo  *info)
{
  g_autoptr (StampFolderItem) folder_item = stamp_folder_item_new (stamp_item_get_account (STAMP_ITEM (self)), info);

  g_signal_connect_object (folder_item, "folder-item-added", G_CALLBACK (on_folder_item_changed), self, G_CONNECT_DEFAULT);

  g_ptr_array_add (items, g_steal_pointer (&folder_item));
}

static void
show_info (StampAccountItem *self,
           CamelFolderInfo  *folder_info)
{
  GListStore *list_store = stamp_item_get_list_store (STAMP_ITEM (self));
  g_autoptr (GPtrArray) items = g_ptr_array_new ();
  g_autoptr (GPtrArray) label_items = g_ptr_array_new ();
  guint old;

  old = g_list_model_get_n_items (G_LIST_MODEL (list_store));

  for (guint idx = 0; idx < old; idx++) {
    StampFolderItem *item = g_list_model_get_item (G_LIST_MODEL (list_store), idx);

    stamp_folder_item_disconnect (item);
  }

  while (folder_info) {
    if (folder_info->full_name && (g_ascii_strcasecmp (folder_info->full_name, "[Google Mail]") == 0 || g_ascii_strcasecmp (folder_info->full_name, "[Gmail]") == 0)) {
      CamelFolderInfo *child = folder_info->child;

      while (child) {
        add_folder_info_item (self, label_items, child);
        child = child->next;
      }
    } else {
      add_folder_info_item (self, items, folder_info);
    }

    folder_info = folder_info->next;
  }

  if (label_items->len > 0) {
    g_autoptr (CamelFolderInfo) labels_info = camel_folder_info_new ();
    g_autoptr (StampFolderItem) labels_item = NULL;
    GListStore *child_store = NULL;
    StampAccount *account = stamp_item_get_account (STAMP_ITEM (self));

    labels_info->display_name = g_strdup (_("Labels"));
    labels_info->full_name = g_strdup (_("Labels"));
    labels_info->flags = CAMEL_FOLDER_NOSELECT | CAMEL_FOLDER_TYPE_NORMAL;

    labels_item = stamp_folder_item_new (account, labels_info);

    stamp_item_set_icon_name (STAMP_ITEM (labels_item), "user-bookmarks-symbolic");
    stamp_item_set_list_store_type (STAMP_ITEM (labels_item), STAMP_TYPE_FOLDER_ITEM);
    child_store = stamp_item_get_list_store (STAMP_ITEM (labels_item));

    for (guint i = 0; i < label_items->len; i++) {
      StampFolderItem *item = g_ptr_array_index (label_items, i);

      g_list_store_append (child_store, STAMP_ITEM (item));
    }

    g_ptr_array_add (items, g_steal_pointer (&labels_item));
  }

  g_list_store_splice (list_store, 0, old, items->pdata, items->len);
}

static gboolean
refresh_folder_main (gpointer user_data);

void
on_offline_store_folder_created (CamelOfflineStore *store,
                                 CamelFolderInfo   *object,
                                 gpointer           user_data);

static void
on_get_folder_info (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);
  CamelStore *store = CAMEL_STORE (source);
  g_autoptr (CamelFolderInfo) folder_info = NULL;
  g_autoptr (GError) error = NULL;

  stamp_item_set_loading (STAMP_ITEM (self), FALSE);

  folder_info = camel_store_get_folder_info_finish (CAMEL_STORE (store), res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning ("Error getting folder: %s", error->message);
      stamp_item_set_error (STAMP_ITEM (self), error);
    } else
      stamp_item_set_error (STAMP_ITEM (self), NULL);

    return;
  }

  if (folder_info) {
    show_info (self, folder_info);
    stamp_account_item_connect_to_account (self);
    g_signal_emit (self, signals[ACCOUNT_ITEM_CHANGED], 0);

    /* Defer folder refresh to give folder items time to load their folders */
    g_clear_handle_id (&self->refresh_folder_handler, g_source_remove);
    self->refresh_folder_handler = g_idle_add (refresh_folder_main, self);
  }
}

void
stamp_account_item_load (StampAccountItem *self)
{
  stamp_item_set_loading (STAMP_ITEM (self), TRUE);

  camel_store_get_folder_info (CAMEL_STORE (self->offline_store),
                               NULL,
                               CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_FAST | CAMEL_STORE_FOLDER_INFO_NO_VIRTUAL,
                               G_PRIORITY_DEFAULT,
                               self->cancellable,
                               on_get_folder_info,
                               self);
}

static void
on_synchronize (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);
  g_autoptr (GError) error = NULL;

  camel_store_synchronize_finish (CAMEL_STORE (self->offline_store), res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("%s: Error synchronizing store: %s", G_STRFUNC, error->message);

    g_object_unref (self);
    return;
  }

  g_object_unref (self);
}

static void
on_service_connect (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);
  g_autoptr (GError) error = NULL;

  camel_service_connect_finish (CAMEL_SERVICE (self->offline_store), res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Error connecting to service: %s", error->message);

    g_object_unref (self);
    return;
  }

  camel_store_synchronize (CAMEL_STORE (self->offline_store), FALSE, G_PRIORITY_DEFAULT, self->cancellable, on_synchronize, self);
}

static void
on_offline_store_set_online (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);
  g_autoptr (GError) error = NULL;

  camel_offline_store_set_online_finish (self->offline_store, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Error setting offline store to online: %s", error->message);

    g_object_unref (self);
    return;
  }

  camel_service_connect (CAMEL_SERVICE (self->offline_store), G_PRIORITY_DEFAULT, self->cancellable, on_service_connect, self);
}

static void
stamp_account_item_connect_to_account (StampAccountItem *self)
{
  GNetworkMonitor *monitor = g_network_monitor_get_default ();

  if (g_cancellable_is_cancelled (self->cancellable))
    return;

  if (!g_network_monitor_get_network_available (monitor))
    return;

  camel_offline_store_set_online (self->offline_store,
                                  TRUE,
                                  G_PRIORITY_DEFAULT,
                                  self->cancellable,
                                  on_offline_store_set_online,
                                  g_object_ref (self));
}

static void
on_network_changed (GNetworkMonitor *monitor,
                    gboolean         network_available,
                    gpointer         user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);

  stamp_account_item_connect_to_account (self);
}

static void
on_offline_store_folder_changed (CamelOfflineStore *store,
                                 gpointer           user_data)
{
  /* StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data); */

  /* stamp_account_item_load (self); */
}

static gboolean
stamp_account_item_find_and_delete_item (StampAccountItem *self,
                                         GListStore       *store,
                                         const gchar      *full_name)
{
  guint list_len = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (guint idx = 0; idx < list_len; idx++) {
    StampFolderItem *item = g_list_model_get_item (G_LIST_MODEL (store), idx);
    GListStore *child_store;

    if (g_strcmp0 (stamp_folder_item_get_full_name (item), full_name) == 0) {
      g_list_store_remove (store, idx);
      return TRUE;
    }

    child_store = stamp_item_get_list_store (STAMP_ITEM (item));
    if (child_store) {
      if (stamp_account_item_find_and_delete_item (self, child_store, full_name))
        return TRUE;
    }
  }

  return FALSE;
}

void
on_offline_store_folder_created (CamelOfflineStore *store,
                                 CamelFolderInfo   *object,
                                 gpointer           user_data)
{
  /* on_offline_store_folder_changed (store, user_data); */
}

static void
on_offline_store_folder_deleted (CamelOfflineStore *store,
                                 CamelFolderInfo   *object,
                                 gpointer           user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);
  const gchar *full_name = object->full_name;
  GListStore *list_store = stamp_item_get_list_store (STAMP_ITEM (self));

  stamp_account_item_find_and_delete_item (self, list_store, full_name);
}

static void
on_offline_store_folder_info_stale (CamelOfflineStore *store,
                                    gpointer           user_data)
{
  on_offline_store_folder_changed (store, user_data);
}

static void
on_offline_store_folder_renamed (CamelOfflineStore *store,
                                 gchar             *object,
                                 CamelFolderInfo   *folder_info,
                                 gpointer           user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);
  StampFolderItem *dest_item = NULL;
  GListStore *list_store = stamp_item_get_list_store (STAMP_ITEM (self));

  dest_item = stamp_account_item_find_item (list_store, object);
  if (!dest_item)
    return;

  stamp_folder_item_set_folder_info (dest_item, folder_info);
}

static void
on_account_changed (StampSession *session,
                    StampAccount *account,
                    gpointer      user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);

  if (stamp_item_get_account (STAMP_ITEM (self)) != account)
    return;

  stamp_item_set_name (STAMP_ITEM (self), stamp_account_get_name (account));
}

static StampFolderItem *
find_folder_item_by_folder (GListStore  *store,
                            CamelFolder *folder)
{
  guint len = g_list_model_get_n_items (G_LIST_MODEL (store));
  StampFolderItem *ret = NULL;

  for (guint idx = 0; idx < len; idx++) {
    g_autoptr (StampFolderItem) folder_item = STAMP_FOLDER_ITEM (g_list_model_get_item (G_LIST_MODEL (store), idx));
    GListStore *child_store = stamp_item_get_list_store (STAMP_ITEM (folder_item));

    if (stamp_folder_item_get_folder (folder_item) == folder) {
      return g_steal_pointer (&folder_item);
    }

    if (child_store)
      ret = find_folder_item_by_folder (child_store, folder);

    if (ret)
      return ret;
  }

  return ret;
}

static gboolean
is_folder_item_in_tree (GListStore      *store,
                        StampFolderItem *target)
{
  guint list_len = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (guint idx = 0; idx < list_len; idx++) {
    g_autoptr (StampFolderItem) item = g_list_model_get_item (G_LIST_MODEL (store), idx);
    GListStore *child_store;

    if (item == target)
      return TRUE;

    child_store = stamp_item_get_list_store (STAMP_ITEM (item));
    if (child_store && is_folder_item_in_tree (child_store, target))
      return TRUE;
  }

  return FALSE;
}

static void
on_refresh (GObject      *source,
            GAsyncResult *result,
            gpointer      user_data);

static gboolean
stamp_account_item_start_next_refresh (StampAccountItem *self)
{
  GListStore *store = stamp_item_get_list_store (STAMP_ITEM (self));

  while (!g_queue_is_empty (self->refresh_queue)) {
    g_autoptr (StampFolderItem) folder_item = g_queue_pop_head (self->refresh_queue);
    CamelFolder *folder = stamp_folder_item_get_folder (folder_item);

    if (CAMEL_IS_FOLDER (folder) && is_folder_item_in_tree (store, folder_item)) {
      self->refresh_queue_running = TRUE;

      g_debug ("%s: Refreshing %s", G_STRFUNC, camel_folder_get_display_name (folder));
      stamp_item_set_loading (STAMP_ITEM (folder_item), TRUE);
      camel_folder_refresh_info (folder, G_PRIORITY_DEFAULT, self->cancellable, on_refresh, self);

      return TRUE;
    }
  }

  self->refresh_queue_running = FALSE;
  return FALSE;
}

static void
on_refresh (GObject      *source,
            GAsyncResult *result,
            gpointer      user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);
  CamelFolder *folder = CAMEL_FOLDER (source);
  g_autoptr (GError) error = NULL;
  GListStore *store = stamp_item_get_list_store (STAMP_ITEM (self));
  g_autoptr (StampFolderItem) folder_item = NULL;

  folder_item = find_folder_item_by_folder (store, folder);
  if (folder_item)
    stamp_item_set_loading (STAMP_ITEM (folder_item), FALSE);

  if (!camel_folder_refresh_info_finish (folder, result, &error)) {
    if (!folder_item || !stamp_item_get_error (STAMP_ITEM (folder_item)))
      g_warning ("Could not refresh folder %s: %s", camel_folder_get_display_name (folder), error->message);

    if (folder_item)
      stamp_item_set_error (STAMP_ITEM (folder_item), error);
    /* Continue */
  } else if (folder_item) {
    stamp_item_set_error (STAMP_ITEM (folder_item), NULL);
  }

  stamp_account_item_start_next_refresh (self);
}

static void
create_queue (StampAccountItem *self,
              GListStore       *store)
{
  guint len = g_list_model_get_n_items (G_LIST_MODEL (store));
  g_autoptr (GPtrArray) priority_folders = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GPtrArray) normal_folders = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint idx = 0; idx < len; idx++) {
    g_autoptr (StampFolderItem) folder_item = STAMP_FOLDER_ITEM (g_list_model_get_item (G_LIST_MODEL (store), idx));
    GListStore *child_store = stamp_item_get_list_store (STAMP_ITEM (folder_item));
    CamelFolder *folder = stamp_folder_item_get_folder (folder_item);
    const gchar *full_name;

    /* First process child folders if present */
    if (child_store) {
      create_queue (self, child_store);
    }

    if (!CAMEL_IS_FOLDER (folder)) {
      continue;
    }

    full_name = stamp_folder_item_get_full_name (folder_item);

    /* Prioritize INBOX and its subfolders (including German names) */
    if (g_str_has_prefix (full_name, "INBOX") ||
        g_strcmp0 (full_name, "Inbox") == 0 ||
        g_strcmp0 (full_name, "INBOX") == 0 ||
        g_strcmp0 (full_name, "Posteingang") == 0 ||
        g_str_has_prefix (full_name, "Posteingang/")) {
      g_ptr_array_add (priority_folders, g_steal_pointer (&folder_item));
    } else {
      g_ptr_array_add (normal_folders, g_steal_pointer (&folder_item));
    }
  }

  /* Add priority folders first (INBOX) */
  for (guint idx = 0; idx < priority_folders->len; idx++) {
    StampFolderItem *folder_item = priority_folders->pdata[idx];
    g_queue_push_tail (self->refresh_queue, g_object_ref (folder_item));
  }

  /* Then normal folders */
  for (guint idx = 0; idx < normal_folders->len; idx++) {
    StampFolderItem *folder_item = normal_folders->pdata[idx];
    g_queue_push_tail (self->refresh_queue, g_object_ref (folder_item));
  }
}

static StampFolderItem *
find_priority_folder_item (StampAccountItem *self)
{
  GList *iter;

  for (iter = self->refresh_queue->head; iter; iter = g_list_next (iter)) {
    StampFolderItem *folder_item = STAMP_FOLDER_ITEM (iter->data);
    const gchar *full_name = stamp_folder_item_get_full_name (folder_item);

    if (g_str_has_prefix (full_name, "INBOX") ||
        g_strcmp0 (full_name, "Inbox") == 0 ||
        g_strcmp0 (full_name, "INBOX") == 0 ||
        g_strcmp0 (full_name, "Posteingang") == 0 ||
        g_str_has_prefix (full_name, "Posteingang/")) {
      return folder_item;
    }
  }

  return NULL;
}

static gboolean
refresh_folder_main (gpointer user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);
  GListStore *store;
  StampFolderItem *folder_item = NULL;

  if (g_cancellable_is_cancelled (self->cancellable)) {
    self->refresh_folder_handler = 0;
    return G_SOURCE_REMOVE;
  }

  store = stamp_item_get_list_store (STAMP_ITEM (self));

  /* Always refresh the first folder in the queue (which is now prioritized) */
  if (!self->refresh_queue_running) {
    /* Clear the queue before adding new items */
    g_queue_clear (self->refresh_queue);

    create_queue (self, store);

    /* On first refresh, prioritize INBOX/Posteingang */
    if (self->first_refresh) {
      self->first_refresh = FALSE;
      folder_item = find_priority_folder_item (self);
      g_debug ("%s: First refresh - looking for INBOX, found: %p", G_STRFUNC, folder_item);

      if (folder_item) {
        if (g_queue_find (self->refresh_queue, folder_item)) {
          g_queue_remove (self->refresh_queue, folder_item);
        }

        g_queue_push_head (self->refresh_queue, folder_item);
      }
    }

    stamp_account_item_start_next_refresh (self);
  } else {
    g_debug ("%s: Refresh already running, queue updated for next iteration", G_STRFUNC);
  }

  self->refresh_folder_handler = 0;
  return G_SOURCE_REMOVE;
}

static gboolean
refresh_timeout (gpointer user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);

  if (g_cancellable_is_cancelled (self->cancellable)) {
    self->refresh_timer_id = 0;
    return G_SOURCE_REMOVE;
  }

  g_clear_handle_id (&self->refresh_folder_handler, g_source_remove);
  refresh_folder_main (self);

  return G_SOURCE_CONTINUE;
}

static void
on_refresh_interval_changed (GSettings *settings,
                             gchar     *key,
                             gpointer   user_data)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (user_data);
  guint interval = MAX (1u, g_settings_get_uint (settings, STAMP_PREFS_MAIL_REFRESH_INTERVAL));

  g_clear_handle_id (&self->refresh_timer_id, g_source_remove);
  self->refresh_timer_id = g_timeout_add_seconds (interval, refresh_timeout, self);
}

static void
stamp_account_item_constructed (GObject *object)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (object);
  GNetworkMonitor *network_monitor = g_network_monitor_get_default ();
  StampMailService *mail_service;

  G_OBJECT_CLASS (stamp_account_item_parent_class)->constructed (object);

  mail_service = stamp_account_get_mail_service (stamp_item_get_account (STAMP_ITEM (self)));
  g_assert (mail_service);

  stamp_item_set_name (STAMP_ITEM (self), stamp_account_get_name (stamp_item_get_account (STAMP_ITEM (self))));

  stamp_item_set_list_store_type (STAMP_ITEM (self), STAMP_TYPE_FOLDER_ITEM);

  self->cancellable = g_cancellable_new ();

  /* Register callbacks for folder changes... */
  self->offline_store = CAMEL_OFFLINE_STORE (stamp_mail_service_get_service (mail_service));
  g_object_ref (self->offline_store);

  g_signal_connect_object (self->offline_store, "folder-created", G_CALLBACK (on_offline_store_folder_created), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (self->offline_store, "folder-deleted", G_CALLBACK (on_offline_store_folder_deleted), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (self->offline_store, "folder-info-stale", G_CALLBACK (on_offline_store_folder_info_stale), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (self->offline_store, "folder-renamed", G_CALLBACK (on_offline_store_folder_renamed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (stamp_session_get_default (), "account-changed", G_CALLBACK (on_account_changed), self, G_CONNECT_DEFAULT);

  /* NetworkMonitor */
  g_signal_connect_object (network_monitor, "network-changed", G_CALLBACK (on_network_changed), self, G_CONNECT_DEFAULT);

  self->refresh_queue = g_queue_new ();

  self->refresh_settings = g_object_ref (STAMP_SETTINGS_MAIL);
  g_signal_connect_object (self->refresh_settings, "changed::" STAMP_PREFS_MAIL_REFRESH_INTERVAL,
                           G_CALLBACK (on_refresh_interval_changed), self, G_CONNECT_DEFAULT);
  on_refresh_interval_changed (self->refresh_settings, NULL, self);
  self->refresh_folder_handler = g_idle_add (refresh_folder_main, self);
}

static void
stamp_account_item_dispose (GObject *object)
{
  StampAccountItem *self = STAMP_ACCOUNT_ITEM (object);

  g_clear_handle_id (&self->refresh_timer_id, g_source_remove);
  if (self->refresh_settings)
    g_signal_handlers_disconnect_by_data (self->refresh_settings, self);
  g_clear_object (&self->refresh_settings);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_handle_id (&self->refresh_folder_handler, g_source_remove);

  g_clear_object (&self->offline_store);

  G_OBJECT_CLASS (stamp_account_item_parent_class)->dispose (object);
}

void
stamp_account_item_class_init (StampAccountItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = stamp_account_item_constructed;
  object_class->dispose = stamp_account_item_dispose;

  signals[ACCOUNT_ITEM_CHANGED] = g_signal_new ("account-item-changed", G_OBJECT_CLASS_TYPE (klass),
                                                G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                                0, NULL, NULL, NULL,
                                                G_TYPE_NONE,
                                                0);
}

void
stamp_account_item_init (StampAccountItem *self)
{
  self->first_refresh = TRUE;
}

StampAccountItem *
stamp_account_item_new (StampAccount *account)
{
  return g_object_new (STAMP_TYPE_ACCOUNT_ITEM, "account", account, NULL);
}
