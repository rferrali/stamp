/*
 * Copyright 2024-2026 Jan-Michael Brummer
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

#include "stamp-photo-cache.h"

#include <string.h>

#include <gdk/gdk.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <libebook/libebook.h>
#include <libedataserver/libedataserver.h>

#include "stamp-account.h"
#include "stamp-bimi.h"
#include "stamp-helper.h"
#include "stamp-session.h"
#include "stamp-settings.h"

#define STAMP_CACHE_TTL_SEC        (7 * 24 * 60 * 60)   /* 1 Week */
#define STAMP_CACHE_NEGATIVE_SUFFIX ".negative"

typedef struct {
  GFunc callback;
  gpointer user_data;
  GCancellable *cancellable;
  gboolean cancelled;
  guint id;
} StampLookupWaiter;

typedef struct {
  struct _StampPhotoCache *cache;
  gchar *email;
  GSList *waiters;
  GCancellable *cancellable;
  gint pending_books;
  gboolean contact_found;
  gint ref_count;
} StampPendingLookup;

typedef struct _StampPhotoCache {
  GHashTable *positive_cache;
  GHashTable *negative_cache;
  GHashTable *pending;
  StampAccount *account;
  gchar *cache_dir;
  guint next_waiter_id;
  GCancellable *cancellable;
} StampPhotoCache;

typedef struct {
  StampPhotoCache *cache;
  gchar *email;
  guint waiter_id;
} StampCancelData;

typedef struct {
  guint generation;
  GCancellable *cancellable;
} StampEmailRow;

typedef struct {
  guint generation;
} StampPhotoToken;

typedef enum {
  STAMP_ADDRESS_BOOK_ALL,
  STAMP_ADDRESS_BOOK_SPECIFIC,
} StampAddressBookMode;

static StampPendingLookup *
stamp_pending_lookup_ref (StampPendingLookup *pending)
{
  g_atomic_int_inc (&pending->ref_count);
  return pending;
}

static void
stamp_pending_lookup_unref (StampPendingLookup *pending)
{
  if (!g_atomic_int_dec_and_test (&pending->ref_count))
    return;

  g_clear_object (&pending->cancellable);
  g_clear_pointer (&pending->email, g_free);
  g_clear_pointer (&pending, g_free);
}

static gchar *
stamp_cache_path_for_email (StampPhotoCache *cache,
                            const gchar     *email,
                            const gchar     *suffix)
{
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, email, -1);
  g_autofree char *path = g_build_filename (cache->cache_dir, hash, NULL);
  return suffix ? g_strconcat (path, suffix, NULL) : g_strdup (path);
}

static gboolean
stamp_disk_cache_is_valid (const gchar *path)
{
  GStatBuf st;
  gint64 age;

  if (g_stat (path, &st) != 0)
    return FALSE;

  age = (gint64)g_get_real_time () / G_USEC_PER_SEC - (gint64)st.st_mtime;
  return age < STAMP_CACHE_TTL_SEC;
}

static GdkTexture *
stamp_disk_cache_load (StampPhotoCache *cache,
                       const gchar     *email)
{
  GdkTexture *texture;
  g_autoptr (GFile) file = NULL;
  g_autofree char *path = stamp_cache_path_for_email (cache, email, NULL);

  if (!stamp_disk_cache_is_valid (path)) {
    return NULL;
  }

  file = g_file_new_for_path (path);
  texture = gdk_texture_new_from_file (file, NULL);
  return texture;
}

static gboolean
stamp_disk_cache_load_negative (StampPhotoCache *cache,
                                const gchar     *email)
{
  g_autofree char *path = stamp_cache_path_for_email (cache, email, STAMP_CACHE_NEGATIVE_SUFFIX);
  return stamp_disk_cache_is_valid (path);
}

static void
stamp_disk_cache_store (StampPhotoCache *cache,
                        const gchar     *email,
                        const guchar    *data,
                        gsize            length)
{
  g_autofree char *path = stamp_cache_path_for_email (cache, email, NULL);
  g_autoptr (GError) error = NULL;

  if (!g_file_set_contents (path, (const char *)data, (gssize)length, &error)) {
    g_warning ("%s: %s", G_STRFUNC, error->message);
  }
}

static void
stamp_disk_cache_store_negative (StampPhotoCache *cache,
                                 const gchar     *email)
{
  g_autofree char *path = stamp_cache_path_for_email (cache, email, STAMP_CACHE_NEGATIVE_SUFFIX);
  g_autoptr (GError) error = NULL;

  if (!g_file_set_contents (path, "", 0, &error)) {
    g_warning ("stamp_disk_cache_store_negative: %s", error->message);
  }
}

void
stamp_disk_cache_purge (StampPhotoCache *cache)
{
  GDir *dir;
  const gchar *name;

  dir = g_dir_open (cache->cache_dir, 0, NULL);
  if (!dir)
    return;

  while ((name = g_dir_read_name (dir))) {
    g_autofree char *path = g_build_filename (cache->cache_dir, name, NULL);

    if (!stamp_disk_cache_is_valid (path))
      g_remove (path);
  }

  g_dir_close (dir);
}

void
stamp_disk_cache_purge_negative (StampPhotoCache *cache)
{
  GDir *dir;
  const gchar *name;

  dir = g_dir_open (cache->cache_dir, 0, NULL);
  if (!dir)
    return;

  while ((name = g_dir_read_name (dir))) {
    g_autofree char *path = g_build_filename (cache->cache_dir, name, NULL);

    if (g_strstr_len (path, -1, STAMP_CACHE_NEGATIVE_SUFFIX))
      g_remove (path);
  }

  g_dir_close (dir);

  g_hash_table_remove_all (cache->negative_cache);
}

StampPhotoCache *
stamp_photo_cache_new (StampAccount *account)
{
  StampPhotoCache *cache = g_new0 (StampPhotoCache, 1);

  cache->positive_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  cache->negative_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  cache->pending = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  cache->account = account;
  cache->cache_dir = g_build_path (G_DIR_SEPARATOR_S, stamp_get_cache_dir (), "avatars", stamp_account_get_uid (account), NULL);
  cache->next_waiter_id = 1;

  if (g_mkdir_with_parents (cache->cache_dir, 0700) != 0) {
    g_warning ("%s: Could not create cache directory: %s", G_STRFUNC, g_strerror (errno));
  }

  return cache;
}

void
stamp_photo_cache_free (StampPhotoCache *cache)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, cache->pending);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    StampPendingLookup *p = value;
    g_cancellable_cancel (p->cancellable);
  }

  g_clear_pointer (&cache->positive_cache, g_hash_table_unref);
  g_clear_pointer (&cache->negative_cache, g_hash_table_unref);
  g_clear_pointer (&cache->pending, g_hash_table_unref);
  g_clear_pointer (&cache->cache_dir, g_free);
  g_clear_pointer (&cache, g_free);
}

static StampCancelData *
stamp_cancel_data_new (StampPhotoCache *cache,
                       const gchar     *email,
                       guint            waiter_id)
{
  StampCancelData *data = g_new0 (StampCancelData, 1);

  data->cache = cache;
  data->email = g_strdup (email);
  data->waiter_id = waiter_id;
  return data;
}

static void
stamp_cancel_data_free (StampCancelData *data)
{
  g_clear_pointer (&data->email, g_free);
  g_clear_pointer (&data, g_free);
}

static gboolean
stamp_on_waiter_cancelled_mainloop (gpointer user_data)
{
  StampCancelData *data = user_data;
  StampPhotoCache *cache = data->cache;
  StampPendingLookup *pending = g_hash_table_lookup (cache->pending, data->email);

  if (!pending)
    return G_SOURCE_REMOVE;

  for (GSList *l = pending->waiters; l; l = l->next) {
    StampLookupWaiter *waiter = l->data;

    if (waiter->id == data->waiter_id) {
      waiter->cancelled = TRUE;
      break;
    }
  }

  return G_SOURCE_REMOVE;
}

static void
stamp_on_waiter_cancelled (GCancellable *cancellable,
                           gpointer      user_data)
{
  g_main_context_invoke (NULL, stamp_on_waiter_cancelled_mainloop, user_data);
}

static GdkTexture *
check_bimi (StampPhotoCache *self,
            const gchar     *email,
            GCancellable    *cancellable)
{
  GdkTexture *texture = NULL;

  if (g_settings_get_boolean (STAMP_SETTINGS_MAIL, STAMP_PREFS_MAIL_LOAD_BIMI_IMAGES)) {
    g_autoptr (GError) error = NULL;
    const gchar *domain = strchr (email, '@');
    g_autofree char *url = NULL;

    if (domain && strlen (domain) > 1) {
      url = stamp_query_bimi_logo (domain + 1);
    }

    if (url) {
      g_autoptr (SoupSession) session = soup_session_new ();
      g_autoptr (SoupMessage) msg = soup_message_new ("GET", url);
      g_autoptr (GBytes) bytes = NULL;

      if (!msg) {
        g_warning ("%s: Could not create soup message for %s", G_STRFUNC, url);
        goto negative;
      }

      bytes = soup_session_send_and_read (session, msg, cancellable, &error);
      if (error) {
        g_debug ("%s:   Could not load file: %s", G_STRFUNC, error->message);
        goto negative;
      }

      texture = gdk_texture_new_from_bytes (bytes, &error);
      if (error) {
        g_debug ("Could not parse texture from %s: %s", url, error->message);
      }

      if (texture) {
        stamp_disk_cache_store (self, email, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
      }
    }
  }

negative:
  return texture;
}

static void
stamp_notify_waiters (StampPendingLookup *pending,
                      GdkTexture         *texture)
{
  for (GSList *l = pending->waiters; l; l = l->next) {
    StampLookupWaiter *waiter = l->data;
    gboolean call = !waiter->cancelled && (!waiter->cancellable || !g_cancellable_is_cancelled (waiter->cancellable));

    if (call && waiter->callback)
      waiter->callback (texture ? g_object_ref (texture) : NULL, waiter->user_data);

    if (waiter->cancellable)
      g_object_unref (waiter->cancellable);

    g_free (waiter);
  }

  g_clear_slist (&pending->waiters, NULL);

  stamp_pending_lookup_unref (pending);
}

static void
on_bimi (GObject      *source,
         GAsyncResult *res,
         gpointer      user_data)
{
  GTask *task = G_TASK (res);
  StampPendingLookup *pending = g_task_get_task_data (task);
  g_autoptr (GdkTexture) texture = NULL;
  g_autoptr (GError) error = NULL;

  texture = g_task_propagate_pointer (task, &error);
  if (error)
    return;

  if (texture) {
    pending->pending_books = 0;
    g_hash_table_insert (pending->cache->positive_cache, g_strdup (pending->email), g_object_ref (texture));
  } else {
    g_hash_table_insert (pending->cache->negative_cache,
                         g_strdup (pending->email), GINT_TO_POINTER (TRUE));
    stamp_disk_cache_store_negative (pending->cache, pending->email);
  }

  g_hash_table_remove (pending->cache->pending, pending->email);
  stamp_notify_waiters (pending, texture);
}

static void
load_bimi (GTask        *task,
           gpointer      source_object,
           gpointer      task_data,
           GCancellable *cancellable)
{
  StampPendingLookup *pending = task_data;
  GdkTexture *texture;

  texture = check_bimi (pending->cache, pending->email, pending->cancellable);

  g_task_return_pointer (task, texture, texture ? g_object_unref : NULL);
}

static void
start_bimi_lookup (StampPendingLookup *pending)
{
  g_autoptr (GTask) task = g_task_new (NULL, pending->cancellable, on_bimi, NULL);

  g_task_set_source_tag (task, start_bimi_lookup);
  stamp_pending_lookup_ref (pending);
  g_task_set_task_data (task, pending, (GDestroyNotify)stamp_pending_lookup_unref);
  g_task_run_in_thread (task, load_bimi);
}

static void
stamp_on_contacts_received (GObject      *source,
                            GAsyncResult *res,
                            gpointer      user_data)
{
  StampPendingLookup *pending = user_data;
  StampPhotoCache *cache = pending->cache;
  g_autoslist (EContact) contacts = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  g_autofree char *domain = NULL;

  if (pending->pending_books == 0) {
    e_book_client_get_contacts_finish (E_BOOK_CLIENT (source), res, &contacts, &error);
    stamp_pending_lookup_unref (pending);
    return;
  }

  if (!e_book_client_get_contacts_finish (E_BOOK_CLIENT (source), res, &contacts, &error)) {
    goto book_done;
  }

  if (contacts) {
    EContact *contact = E_CONTACT (contacts->data);
    EContactPhoto *photo = e_contact_get (contact, E_CONTACT_PHOTO);

    pending->contact_found = TRUE;

    if (!photo) {
      g_debug ("%s: Contact found for '%s' but no photo field", G_STRFUNC, pending->email);
    } else if (photo->type == E_CONTACT_PHOTO_TYPE_INLINED) {
      g_autoptr (GBytes) bytes = g_bytes_new (photo->data.inlined.data, photo->data.inlined.length);

      g_debug ("%s: Found inline photo for '%s'", G_STRFUNC, pending->email);

      texture = gdk_texture_new_from_bytes (bytes, NULL);
      if (texture)
        stamp_disk_cache_store (cache, pending->email, photo->data.inlined.data, photo->data.inlined.length);
      else
        g_warning ("%s: Could not parse inline photo for '%s'", G_STRFUNC, pending->email);

      e_contact_photo_free (photo);
    } else if (photo->type == E_CONTACT_PHOTO_TYPE_URI) {
      g_debug ("%s: Found URI photo for '%s': %s", G_STRFUNC, pending->email, photo->data.uri);

      if (g_str_has_prefix (photo->data.uri, "file://")) {
        g_autoptr (GFile) file = g_file_new_for_uri (photo->data.uri);

        texture = gdk_texture_new_from_file (file, &error);
        if (texture) {
          g_autofree char *data = NULL;
          gsize length = 0;

          g_debug ("%s: Successfully loaded file URI photo for '%s'", G_STRFUNC, pending->email);
          if (g_file_load_contents (file, NULL, &data, &length, NULL, NULL))
            stamp_disk_cache_store (cache, pending->email, (guchar *)data, length);
        } else {
          g_warning ("%s: Could not load photo from uri '%s': %s", G_STRFUNC, photo->data.uri, error->message);
        }
      }

      e_contact_photo_free (photo);
    }
  }

  if (texture) {
    pending->pending_books = 0;
    g_hash_table_insert (cache->positive_cache, g_strdup (pending->email), g_object_ref (texture));
    g_hash_table_remove (cache->pending, pending->email);
    stamp_notify_waiters (pending, texture);
    stamp_pending_lookup_unref (pending);
    return;
  }

book_done:

  pending->pending_books--;
  if (pending->pending_books > 0) {
    stamp_pending_lookup_unref (pending);
    return;
  }

  /* BIMI is only used for senders which are not part of any address book */
  if (!pending->contact_found) {
    start_bimi_lookup (pending);
    stamp_pending_lookup_unref (pending);
    return;
  }

  g_debug ("%s: All books exhausted, creating NEGATIVE cache for '%s'", G_STRFUNC, pending->email);
  g_hash_table_insert (cache->negative_cache,
                       g_strdup (pending->email), GINT_TO_POINTER (TRUE));
  stamp_disk_cache_store_negative (cache, pending->email);
  g_hash_table_remove (cache->pending, pending->email);
  stamp_notify_waiters (pending, NULL);
  stamp_pending_lookup_unref (pending);
}

/**
 * Try to lookup a photo for a given search string (mail or name) using several (complex) caches:
 *
 * - positive cache: A cache holding information whether we already tried to lookup the search string and found an image
 * - negative cache: A cache holding information whether we already tried to lookup the search string but couldn't find a result
 * - disk cache: A local disk cache in case the previous cache do not contain information yet but are stored on disk
 *
 * In case all caches failed we try to find a photo in parallel and create a pending structure for multiple request so we
 * do not query the same search string multiple times.
 *
 * @book_uid: if specified we are only using this address book as photo source, otherwise we are scanning all books
 */
void
stamp_photo_cache_lookup_async (StampPhotoCache *self,
                                const gchar     *search_string,
                                const gchar     *book_uid,
                                GCancellable    *cancellable,
                                GFunc            callback,
                                gpointer         user_data)
{
  GdkTexture *cached;
  GdkTexture *disk_texture;
  StampLookupWaiter *waiter;
  StampPendingLookup *pending;
  GSList *books = NULL;
  g_autofree char *query_string = NULL;
  EBookQuery *query[2];
  g_autoptr (EBookQuery) or_query = NULL;

  g_return_if_fail (g_main_context_is_owner (g_main_context_default ()));

  if (!search_string) {
    if (callback)
      callback (NULL, user_data);
    return;
  }

  if (!self) {
    if (callback)
      callback (NULL, user_data);
    return;
  }

  cached = g_hash_table_lookup (self->positive_cache, search_string);
  if (cached) {
    if (callback)
      callback (g_object_ref (cached), user_data);
    return;
  }

  if (g_hash_table_contains (self->negative_cache, search_string)) {
    if (callback)
      callback (NULL, user_data);
    return;
  }

  if (stamp_disk_cache_load_negative (self, search_string)) {
    g_hash_table_insert (self->negative_cache, g_strdup (search_string), GINT_TO_POINTER (TRUE));

    if (callback)
      callback (NULL, user_data);
    return;
  }

  disk_texture = stamp_disk_cache_load (self, search_string);
  if (disk_texture) {
    g_hash_table_insert (self->positive_cache, g_strdup (search_string), g_object_ref (disk_texture));

    if (callback)
      callback (disk_texture, user_data);
    return;
  }

  waiter = g_new0 (StampLookupWaiter, 1);
  waiter->callback = callback;
  waiter->user_data = user_data;
  waiter->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  waiter->cancelled = FALSE;
  waiter->id = self->next_waiter_id++;

  pending = g_hash_table_lookup (self->pending, search_string);
  if (pending) {
    pending->waiters = g_slist_append (pending->waiters, waiter);
    if (cancellable)
      g_cancellable_connect (cancellable, G_CALLBACK (stamp_on_waiter_cancelled), stamp_cancel_data_new (self, search_string, waiter->id), (GDestroyNotify)stamp_cancel_data_free);
    return;
  }

  {
    GPtrArray *account_books = stamp_account_get_books (self->account);

    for (gint idx = 0; idx < account_books->len; idx++) {
      StampContactsService *service = g_ptr_array_index (account_books, idx);

      if (!stamp_contacts_service_get_enabled (service))
        continue;

      books = g_slist_append (books, stamp_contacts_service_get_client (service));
    }
  }

  if (!books) {
    g_debug ("%s: No enabled books for '%s', trying BIMI only", G_STRFUNC, search_string);

    pending = g_new0 (StampPendingLookup, 1);
    pending->cache = self;
    pending->email = g_strdup (search_string);
    pending->waiters = g_slist_append (NULL, waiter);
    pending->cancellable = g_cancellable_new ();
    pending->ref_count = 1;
    pending->pending_books = 1;

    g_hash_table_insert (self->pending, g_strdup (search_string), pending);

    if (cancellable)
      g_cancellable_connect (cancellable, G_CALLBACK (stamp_on_waiter_cancelled), stamp_cancel_data_new (self, search_string, waiter->id), (GDestroyNotify)stamp_cancel_data_free);

    start_bimi_lookup (pending);
    return;
  }

  pending = g_new0 (StampPendingLookup, 1);
  pending->cache = self;
  pending->email = g_strdup (search_string);
  pending->waiters = g_slist_append (NULL, waiter);
  pending->cancellable = g_cancellable_new ();
  pending->ref_count = 1;
  pending->pending_books = g_slist_length (books);

  g_hash_table_insert (self->pending, g_strdup (search_string), pending);

  if (cancellable)
    g_cancellable_connect (cancellable, G_CALLBACK (stamp_on_waiter_cancelled), stamp_cancel_data_new (self, search_string, waiter->id), (GDestroyNotify)stamp_cancel_data_free);

  /* We might have contacts in our address book which do not contain a mail address, so we need to check full name as well... */
  query[0] = e_book_query_field_test (E_CONTACT_EMAIL, E_BOOK_QUERY_IS, search_string);
  query[1] = e_book_query_field_test (E_CONTACT_FULL_NAME, E_BOOK_QUERY_IS, search_string);

  or_query = e_book_query_or (2, query, TRUE);

  query_string = e_book_query_to_string (or_query);

  for (GSList *l = books; l; l = l->next) {
    stamp_pending_lookup_ref (pending);
    e_book_client_get_contacts (E_BOOK_CLIENT (l->data),
                                query_string,
                                pending->cancellable,
                                stamp_on_contacts_received,
                                pending);
  }

  g_slist_free (books);
}
