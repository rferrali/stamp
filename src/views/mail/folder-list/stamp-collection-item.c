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

#include "stamp-collection-item.h"

#include <glib/gi18n.h>

#include "stamp-item.h"
#include "stamp-settings.h"

struct _StampCollectionItem {
  StampItem parent_instance;

  StampCollectionType collection_type;
  GList *sources;
  GCancellable *cancellable;
  guint unread;
};

G_DEFINE_FINAL_TYPE (StampCollectionItem, stamp_collection_item, STAMP_TYPE_ITEM);

typedef enum {
  PROP_TYPE = 1,
  PROP_UNREAD,
} StampCollectionItemProps;

static GParamSpec *properties[PROP_UNREAD + 1];

enum {
  SOURCES_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
stamp_collection_item_update_unread (StampCollectionItem *self)
{
  guint unread = 0;

  for (GList *iter = self->sources; iter; iter = iter->next) {
    StampCollectionSource *source = iter->data;

    if (source->loaded)
      unread += source->unread;
  }

  if (self->unread != unread) {
    self->unread = unread;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_UNREAD]);
  }
}

static void
on_source_folder_changed (CamelFolder           *folder,
                          CamelFolderChangeInfo *changes,
                          gpointer               user_data)
{
  StampCollectionItem *self = STAMP_COLLECTION_ITEM (user_data);
  CamelFolderSummary *summary;

  if (!(changes->uid_added || changes->uid_removed || changes->uid_changed))
    return;

  summary = camel_folder_get_folder_summary (folder);

  for (GList *iter = self->sources; iter; iter = iter->next) {
    StampCollectionSource *source = iter->data;

    if (source->folder == folder) {
      source->unread = camel_folder_summary_get_unread_count (summary);
      source->loaded = TRUE;
      break;
    }
  }

  stamp_collection_item_update_unread (self);
}

static void
on_get_folder (GObject      *source,
               GAsyncResult *res,
               gpointer      user_data)
{
  g_autoptr (StampCollectionItem) self = STAMP_COLLECTION_ITEM (user_data);
  CamelStore *store = CAMEL_STORE (source);
  g_autoptr (CamelFolder) folder = NULL;
  g_autoptr (GError) error = NULL;

  folder = camel_store_get_folder_finish (store, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Could not load collection folder: %s", error->message);
    return;
  }

  for (GList *iter = self->sources; iter; iter = iter->next) {
    StampCollectionSource *cur_source = iter->data;
    StampMailService *mail_service = stamp_account_get_mail_service (cur_source->account);

    if (mail_service && stamp_mail_service_get_service (mail_service) == CAMEL_SERVICE (store) &&
        g_strcmp0 (cur_source->full_name, camel_folder_get_full_name (folder)) == 0) {
      if (cur_source->folder != folder) {
        if (cur_source->folder)
          g_signal_handlers_disconnect_by_func (cur_source->folder, on_source_folder_changed, self);
        g_set_object (&cur_source->folder, folder);
        g_signal_connect_object (folder, "changed", G_CALLBACK (on_source_folder_changed), self, G_CONNECT_DEFAULT);
      }

      {
        CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);

        cur_source->unread = camel_folder_summary_get_unread_count (summary);
        cur_source->loaded = TRUE;
      }
      break;
    }
  }

  stamp_collection_item_update_unread (self);
}

static void
stamp_collection_item_load_folders (StampCollectionItem *self)
{
  for (GList *iter = self->sources; iter; iter = iter->next) {
    StampCollectionSource *source = iter->data;
    StampMailService *mail_service;
    CamelStore *store;

    if (source->folder)
      continue;

    mail_service = stamp_account_get_mail_service (source->account);
    if (!mail_service || !stamp_mail_service_get_enabled (mail_service))
      continue;

    store = CAMEL_STORE (stamp_mail_service_get_service (mail_service));
    camel_store_get_folder (store,
                            source->full_name,
                            CAMEL_STORE_FOLDER_NONE,
                            G_PRIORITY_DEFAULT,
                            self->cancellable,
                            on_get_folder,
                            g_object_ref (self));
  }
}

void
stamp_collection_item_load (StampCollectionItem *self)
{
  stamp_collection_item_load_folders (self);
}

static void
stamp_collection_item_set_property (GObject      *object,
                                    guint         property_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  StampCollectionItem *self = STAMP_COLLECTION_ITEM (object);

  switch ((StampCollectionItemProps)property_id) {
    case PROP_TYPE:
      self->collection_type = g_value_get_int (value);
      break;
    case PROP_UNREAD:
      break;
  }
}

static void
stamp_collection_item_get_property (GObject    *object,
                                    guint       property_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  StampCollectionItem *self = STAMP_COLLECTION_ITEM (object);

  switch ((StampCollectionItemProps)property_id) {
    case PROP_TYPE:
      g_value_set_int (value, self->collection_type);
      break;
    case PROP_UNREAD:
      g_value_set_uint (value, self->unread);
      break;
  }
}

static void
stamp_collection_item_finalize (GObject *object)
{
  StampCollectionItem *self = STAMP_COLLECTION_ITEM (object);

  for (GList *iter = self->sources; iter; iter = iter->next) {
    StampCollectionSource *source = iter->data;

    if (source->folder)
      g_object_unref (source->folder);
    g_clear_pointer (&source->full_name, g_free);
    g_clear_object (&source->account);
    g_free (source);
  }

  g_clear_list (&self->sources, NULL);

  G_OBJECT_CLASS (stamp_collection_item_parent_class)->finalize (object);
}

static void
stamp_collection_item_dispose (GObject *object)
{
  StampCollectionItem *self = STAMP_COLLECTION_ITEM (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (stamp_collection_item_parent_class)->dispose (object);
}

static void
stamp_collection_item_class_init (StampCollectionItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = stamp_collection_item_set_property;
  object_class->get_property = stamp_collection_item_get_property;
  object_class->finalize = stamp_collection_item_finalize;
  object_class->dispose = stamp_collection_item_dispose;

  signals[SOURCES_CHANGED] =
    g_signal_new ("sources-changed",
                  G_OBJECT_CLASS_TYPE (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE,
                  0);

  properties[PROP_TYPE] =
    g_param_spec_int ("collection-type",
                      NULL, NULL,
                      0, G_MAXINT,
                      0,
                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  properties[PROP_UNREAD] =
    g_param_spec_uint ("unread",
                       NULL, NULL,
                       0,
                       G_MAXUINT,
                       0,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);
}

static void
stamp_collection_item_init (StampCollectionItem *self)
{
  self->cancellable = g_cancellable_new ();
}

StampCollectionItem *
stamp_collection_item_new (StampCollectionType type)
{
  return g_object_new (STAMP_TYPE_COLLECTION_ITEM,
                       "collection-type", type,
                       NULL);
}

StampCollectionType
stamp_collection_item_get_collection_type (StampCollectionItem *self)
{
  return self->collection_type;
}

void
stamp_collection_item_add_source (StampCollectionItem *self,
                                  StampAccount        *account,
                                  const gchar         *full_name)
{
  StampCollectionSource *source = g_new0 (StampCollectionSource, 1);

  source->account = g_object_ref (account);
  source->full_name = g_strdup (full_name);

  self->sources = g_list_append (self->sources, source);

  g_signal_emit (self, signals[SOURCES_CHANGED], 0);
}

void
stamp_collection_item_remove_source (StampCollectionItem *self,
                                     StampAccount        *account)
{
  GList *iter = self->sources;

  while (iter) {
    StampCollectionSource *source = iter->data;
    GList *next = iter->next;

    if (source->account == account) {
      if (source->folder)
        g_object_unref (source->folder);
      g_clear_pointer (&source->full_name, g_free);
      g_clear_object (&source->account);
      g_free (source);

      self->sources = g_list_delete_link (self->sources, iter);
    }

    iter = next;
  }

  stamp_collection_item_update_unread (self);

  g_signal_emit (self, signals[SOURCES_CHANGED], 0);
}

void
stamp_collection_item_clear (StampCollectionItem *self)
{
  while (self->sources) {
    StampCollectionSource *source = self->sources->data;

    if (source->folder)
      g_object_unref (source->folder);
    g_clear_pointer (&source->full_name, g_free);
    g_clear_object (&source->account);
    g_free (source);

    self->sources = g_list_delete_link (self->sources, self->sources);
  }

  stamp_collection_item_update_unread (self);

  g_signal_emit (self, signals[SOURCES_CHANGED], 0);
}

GList *
stamp_collection_item_get_sources (StampCollectionItem *self)
{
  return self->sources;
}
