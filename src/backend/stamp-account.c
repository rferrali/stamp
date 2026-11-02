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

#include "stamp-account.h"
#include "stamp-photo-cache.h"
#include "stamp-session.h"

struct _StampMailService {
  CamelService *service;

  CamelSession *session;
  CamelTransport *transport;
  CamelFolder *trash_folder;
  CamelFolder *sent_folder;
  CamelFolder *drafts_folder;
  CamelFolder *junk_folder;
  CamelFolder *archives_folder;
  CamelFolder *templates_folder;
  ESource *source;
  ESource *transport_source;
  ESource *identity_source;
  CamelInternetAddress *address;
  gboolean enabled;
  GHashTable *aliases;
  ESourceRegistry *registry;
};

struct _StampContactsService {
  EBookClient *client;
  ESource *source;
  gboolean enabled;
};

struct _StampAccount {
  GObject parent_instance;

  gchar *uid;
  gchar *display_name;
  ESourceRegistry *registry;

  ESource *collection;
  StampMailService *mail;
  GCancellable *mail_cancellable;
  GPtrArray *address_books;

  GCancellable *cancellable;

  StampPhotoCache *photo_cache;
  GList *categories;
  const gchar *color_class;
  GSettings *settings;
};

G_DEFINE_FINAL_TYPE (StampAccount, stamp_account, G_TYPE_OBJECT);

typedef struct {
  StampAccount *account;
  GTask *parent_task;
  gint pending;
  GError *error;
  GMutex lock;
} InitContext;

enum {
  MAIL_ADDED,
  MAIL_REMOVED,
  BOOK_ADDED,
  BOOK_REMOVED,
  COLOR_CHANGED,
  LAST_SIGNAL,
};

static gint signals[LAST_SIGNAL] = { 0 };

static void
init_context_free (InitContext *ctx)
{
  g_mutex_clear (&ctx->lock);
  g_clear_error (&ctx->error);
  g_clear_object (&ctx->parent_task);
  g_clear_pointer (&ctx, g_free);
}

static void
init_context_finish_one (InitContext *ctx)
{
  gint remaining;

  g_mutex_lock (&ctx->lock);
  remaining = --ctx->pending;
  g_mutex_unlock (&ctx->lock);

  if (remaining > 0)
    return;

  if (ctx->error)
    g_task_return_error (ctx->parent_task, g_steal_pointer (&ctx->error));
  else
    g_task_return_boolean (ctx->parent_task, TRUE);

  g_clear_pointer (&ctx, init_context_free);
}

static void
init_context_set_error (InitContext *ctx,
                        GError      *error)
{
  g_mutex_lock (&ctx->lock);
  if (!ctx->error)
    g_propagate_error (&ctx->error, error);
  else
    g_clear_error (&error);

  g_mutex_unlock (&ctx->lock);
}

static void
stamp_contacts_service_free (gpointer data)
{
  StampContactsService *service = data;

  g_clear_object (&service->client);
  g_clear_object (&service->source);
  g_free (service);
}

static void
stamp_account_init (StampAccount *self)
{
  self->cancellable = g_cancellable_new ();

  self->address_books = g_ptr_array_new_with_free_func (stamp_contacts_service_free);
}

static void
stamp_account_mail_service_free (gpointer data)
{
  StampMailService *service = data;

  g_clear_object (&service->address);
  g_clear_object (&service->transport);
  g_clear_object (&service->session);
  g_clear_object (&service->source);
  g_clear_object (&service->trash_folder);
  g_clear_object (&service->sent_folder);
  g_clear_object (&service->drafts_folder);
  g_clear_object (&service->junk_folder);
  g_clear_object (&service->archives_folder);
  g_clear_object (&service->templates_folder);
  g_clear_pointer (&service, g_free);
}

static void
stamp_account_dispose (GObject *object)
{
  StampAccount *self = STAMP_ACCOUNT (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_cancellable_cancel (self->mail_cancellable);
  g_clear_object (&self->mail_cancellable);

  g_clear_pointer (&self->uid, g_free);
  g_clear_pointer (&self->display_name, g_free);
  g_clear_object (&self->collection);
  g_clear_object (&self->registry);

  g_clear_pointer (&self->mail, stamp_account_mail_service_free);
  g_clear_pointer (&self->address_books, g_ptr_array_unref);

  g_clear_pointer (&self->photo_cache, stamp_photo_cache_free);

  if (self->settings) {
    g_signal_handlers_disconnect_by_data (self->settings, self);
    g_clear_object (&self->settings);
  }

  G_OBJECT_CLASS (stamp_account_parent_class)->dispose (object);
}

static void
stamp_account_class_init (StampAccountClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = stamp_account_dispose;

  signals[MAIL_ADDED] = g_signal_new ("mail-added", G_OBJECT_CLASS_TYPE (klass),
                                      G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                      0, NULL, NULL, NULL,
                                      G_TYPE_NONE,
                                      2, STAMP_TYPE_ACCOUNT,
                                      G_TYPE_POINTER);
  signals[MAIL_REMOVED] = g_signal_new ("mail-removed", G_OBJECT_CLASS_TYPE (klass),
                                        G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL,
                                        G_TYPE_NONE,
                                        2, STAMP_TYPE_ACCOUNT,
                                        G_TYPE_POINTER);
  signals[BOOK_ADDED] = g_signal_new ("book-added", G_OBJECT_CLASS_TYPE (klass),
                                      G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                      0, NULL, NULL, NULL,
                                      G_TYPE_NONE,
                                      2, STAMP_TYPE_ACCOUNT,
                                      G_TYPE_POINTER);
  signals[BOOK_REMOVED] = g_signal_new ("book-removed", G_OBJECT_CLASS_TYPE (klass),
                                        G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL,
                                        G_TYPE_NONE,
                                        2, STAMP_TYPE_ACCOUNT,
                                        G_TYPE_POINTER);
  signals[COLOR_CHANGED] = g_signal_new ("color-changed", G_OBJECT_CLASS_TYPE (klass),
                                         G_SIGNAL_RUN_FIRST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE,
                                         0);
}

#define NUMBER_OF_COLORS 14

static const gchar *ACCOUNT_COLOR_CLASSES[NUMBER_OF_COLORS] = {
  "color1",
  "color2",
  "color3",
  "color4",
  "color5",
  "color6",
  "color7",
  "color8",
  "color9",
  "color10",
  "color11",
  "color12",
  "color13",
  "color14",
};

static void
update_color_class (StampAccount *self)
{
  g_autofree gchar *badge_color = NULL;
  const gchar *text;
  guint color_index;
  StampSession *session;

  if (self->settings)
    badge_color = g_settings_get_string (self->settings, "badge-color");

  if (badge_color && *badge_color && g_strcmp0 (badge_color, "auto") != 0) {
    for (guint i = 0; i < NUMBER_OF_COLORS; i++) {
      if (g_strcmp0 (badge_color, ACCOUNT_COLOR_CLASSES[i]) == 0) {
        self->color_class = ACCOUNT_COLOR_CLASSES[i];
        return;
      }
    }
  }

  text = self->display_name ? self->display_name : self->uid;
  color_index = (text && *text) ? (g_str_hash (text) % NUMBER_OF_COLORS) : 0;

  session = stamp_session_get_default ();
  if (session) {
    GList *accounts = stamp_session_get_accounts (session);
    gboolean used[NUMBER_OF_COLORS] = { FALSE };
    guint probes = 0;
    GList *l;

    /* First pass: reserve colors explicitly set to a custom color by other accounts */
    for (l = accounts; l; l = l->next) {
      StampAccount *other = STAMP_ACCOUNT (l->data);
      if (other != self && other->settings) {
        g_autofree gchar *other_setting = g_settings_get_string (other->settings, "badge-color");
        if (other_setting && *other_setting && g_strcmp0 (other_setting, "auto") != 0) {
          for (guint i = 0; i < NUMBER_OF_COLORS; i++) {
            if (g_strcmp0 (other_setting, ACCOUNT_COLOR_CLASSES[i]) == 0) {
              used[i] = TRUE;
              break;
            }
          }
        }
      }
    }

    /* Second pass: reserve colors already used by preceding accounts in the list */
    for (l = accounts; l; l = l->next) {
      StampAccount *other = STAMP_ACCOUNT (l->data);
      if (other == self)
        break;

      if (other->color_class) {
        for (guint i = 0; i < NUMBER_OF_COLORS; i++) {
          if (g_strcmp0 (other->color_class, ACCOUNT_COLOR_CLASSES[i]) == 0) {
            used[i] = TRUE;
            break;
          }
        }
      }
    }

    /* Probe forward to next available color in case of collision */
    while (used[color_index] && probes < NUMBER_OF_COLORS) {
      color_index = (color_index + 1) % NUMBER_OF_COLORS;
      probes++;
    }
  }

  self->color_class = ACCOUNT_COLOR_CLASSES[color_index];
}

void
stamp_account_update_color (StampAccount *self)
{
  const gchar *old_color;

  g_return_if_fail (STAMP_IS_ACCOUNT (self));

  old_color = self->color_class;
  update_color_class (self);

  if (g_strcmp0 (old_color, self->color_class) != 0)
    g_signal_emit (self, signals[COLOR_CHANGED], 0);
}

static void
update_all_accounts_colors (void)
{
  StampSession *session = stamp_session_get_default ();
  GList *accounts;
  GList *l;

  if (!session)
    return;

  accounts = stamp_session_get_accounts (session);
  for (l = accounts; l; l = l->next) {
    StampAccount *acc = STAMP_ACCOUNT (l->data);
    stamp_account_update_color (acc);
  }
}

static void
on_settings_badge_color_changed (GSettings    *settings,
                                 const gchar  *key,
                                 StampAccount *self)
{
  update_all_accounts_colors ();
}

StampAccount *
stamp_account_new (ESource         *collection,
                   ESourceRegistry *registry)
{
  g_autofree gchar *settings_path = NULL;
  StampAccount *self = g_object_new (STAMP_TYPE_ACCOUNT, NULL);

  self->uid = g_strdup (e_source_get_uid (collection));
  self->display_name = g_strdup (e_source_get_display_name (collection));
  self->registry = g_object_ref (registry);
  self->collection = g_object_ref (collection);

  settings_path = g_strdup_printf ("/org/tabos/stamp/mail/accounts/%s/", self->uid);
  self->settings = g_settings_new_with_path ("org.tabos.stamp.mail.accounts", settings_path);
  g_signal_connect_object (self->settings, "changed::badge-color",
                           G_CALLBACK (on_settings_badge_color_changed), self, G_CONNECT_DEFAULT);

  update_color_class (self);

  self->photo_cache = stamp_photo_cache_new (self);

  return self;
}

const gchar *
stamp_account_get_name (StampAccount *self)
{
  return self->display_name;
}

const gchar *
stamp_account_get_uid (StampAccount *self)
{
  return self->uid;
}

const gchar *
stamp_account_get_color_class (StampAccount *self)
{
  g_return_val_if_fail (STAMP_IS_ACCOUNT (self), ACCOUNT_COLOR_CLASSES[0]);

  if (!self->color_class)
    update_color_class (self);

  return self->color_class;
}

void
stamp_account_set_badge_color (StampAccount *self,
                               const gchar  *badge_color)
{
  g_return_if_fail (STAMP_IS_ACCOUNT (self));

  if (!self->settings)
    return;

  if (!badge_color || !*badge_color)
    badge_color = "auto";

  g_settings_set_string (self->settings, "badge-color", badge_color);
}

gchar *
stamp_account_get_badge_color (StampAccount *self)
{
  g_return_val_if_fail (STAMP_IS_ACCOUNT (self), g_strdup ("auto"));

  if (!self->settings)
    return g_strdup ("auto");

  return g_settings_get_string (self->settings, "badge-color");
}

static EBookClient *
stamp_account_get_book_client (StampAccount *self)
{
  if (self->address_books->len != 0) {
    StampContactsService *service;

#if 0
    /* FIXME: Hack for our company, remove once setting of default book is enabled */
    for (gint idx = 0; idx < self->address_books->len; idx++) {
      const gchar *name;

      service = g_ptr_array_index (self->address_books, idx);
      name = e_source_get_display_name (service->source);

      if (g_strcmp0 (name, "Organisationsbenutzer") == 0)
        return service->client;
    }
#endif

    service = g_ptr_array_index (self->address_books, 0);
    return service->client;
  }

  return NULL;
}

void
stamp_account_get_photo (StampAccount *self,
                         const gchar  *sender,
                         GCancellable *cancellable,
                         GFunc         callback,
                         gpointer      user_data)
{
  stamp_photo_cache_lookup_async (self->photo_cache, sender, NULL, cancellable, callback, user_data);
}

gpointer
stamp_account_get_photo_finish (StampAccount  *session,
                                GAsyncResult  *result,
                                GError       **error)
{
  gpointer ptr;

  g_assert (g_task_is_valid (result, session));
  g_assert (error && !*error);

  ptr = g_task_propagate_pointer (G_TASK (result), error);

  return ptr;
}

static void
free_contact_slist (gpointer data)
{
  g_slist_free_full (data, g_object_unref);
}

typedef struct {
  GCancellable *cancellable;
  GCancellable *parent;
  gulong cancelled_id;
  guint timeout_id;
  gboolean completed;
} ContactSearch;

static void
contact_search_free (ContactSearch *search)
{
  g_clear_handle_id (&search->timeout_id, g_source_remove);
  if (search->parent && search->cancelled_id)
    g_cancellable_disconnect (search->parent, search->cancelled_id);
  g_clear_object (&search->parent);
  g_clear_object (&search->cancellable);
  g_free (search);
}

static void
on_contact_search_cancelled (GCancellable *parent,
                             gpointer      user_data)
{
  g_cancellable_cancel (G_CANCELLABLE (user_data));
}

static gboolean
on_contact_search_timeout (gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  ContactSearch *search = g_task_get_task_data (task);
  ESource *source = e_client_get_source (E_CLIENT (g_task_get_source_object (task)));

  search->timeout_id = 0;
  search->completed = TRUE;
  g_cancellable_cancel (search->cancellable);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                           "Searching address book '%s' timed out after 10 seconds",
                           e_source_get_display_name (source));
  return G_SOURCE_REMOVE;
}

static void
on_contact_search_ready (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  ContactSearch *search = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;
  GSList *contacts = NULL;

  e_book_client_get_contacts_finish (E_BOOK_CLIENT (source), res, &contacts, &error);
  if (search->completed) {
    free_contact_slist (contacts);
    return;
  }

  search->completed = TRUE;
  g_clear_handle_id (&search->timeout_id, g_source_remove);
  if (error) {
    free_contact_slist (contacts);
    g_task_return_error (task, g_steal_pointer (&error));
  } else {
    g_task_return_pointer (task, contacts, free_contact_slist);
  }
}

static gchar *
build_contact_search_query (const gchar *search_text,
                            gboolean     directory)
{
  g_auto (GStrv) words = NULL;
  g_autoptr (GPtrArray) conditions = g_ptr_array_new ();
  g_autoptr (EBookQuery) query = NULL;
  guint longest = 0;

  if (!search_text || !*search_text)
    return NULL;
  words = g_strsplit_set (search_text, " \t\r\n", -1);
  for (guint i = 1; words[i]; i++) {
    if (g_utf8_strlen (words[i], -1) > g_utf8_strlen (words[longest], -1))
      longest = i;
  }

  /* The Microsoft 365 backend uses the first term for its server query.
   * Short prefixes can fetch many pages and a photo per organizational user.
   */
  if (!*words[longest] || (directory && g_utf8_strlen (words[longest], -1) < 3))
    return NULL;

  g_ptr_array_add (conditions, e_book_query_field_exists (E_CONTACT_EMAIL));
  for (guint i = 0; words[i]; i++) {
    guint index = i == 0 ? longest : (i == longest ? 0 : i);
    EBookQuery *fields[2];

    if (!*words[index])
      continue;
    fields[0] = e_book_query_field_test (E_CONTACT_FULL_NAME, E_BOOK_QUERY_CONTAINS, words[index]);
    fields[1] = e_book_query_field_test (E_CONTACT_EMAIL, E_BOOK_QUERY_CONTAINS, words[index]);
    g_ptr_array_add (conditions, e_book_query_or (G_N_ELEMENTS (fields), fields, TRUE));
  }

  query = e_book_query_and (conditions->len, (EBookQuery **)conditions->pdata, TRUE);
  return e_book_query_to_string (query);
}

void
stamp_account_search_contacts (StampAccount        *self,
                               EBookClient         *client,
                               const gchar         *search_text,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  EBookClient *book_client = client ? client : stamp_account_get_book_client (self);
  g_autoptr (GTask) task = g_task_new (book_client, cancellable, callback, user_data);
  g_autofree gchar *sexp = NULL;
  ContactSearch *search;
  gboolean directory;

  g_task_set_source_tag (task, stamp_account_search_contacts);
  if (!book_client) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "No address book is connected");
    return;
  }

  directory = stamp_account_address_book_is_search_only (self, E_CLIENT (book_client));
  sexp = build_contact_search_query (search_text, directory);
  if (!sexp) {
    g_task_return_pointer (task, NULL, free_contact_slist);
    return;
  }

  search = g_new0 (ContactSearch, 1);
  search->cancellable = g_cancellable_new ();
  if (cancellable) {
    search->parent = g_object_ref (cancellable);
    search->cancelled_id = g_cancellable_connect (cancellable,
                                                  G_CALLBACK (on_contact_search_cancelled),
                                                  g_object_ref (search->cancellable), g_object_unref);
  }
  g_task_set_task_data (task, search, (GDestroyNotify)contact_search_free);
  /* if (directory) */
  /*   search->timeout_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, 10, on_contact_search_timeout, */
  /*                                                   g_object_ref (task), g_object_unref); */
  e_book_client_get_contacts (book_client, sexp, search->cancellable,
                              on_contact_search_ready, g_object_ref (task));
}

GSList *
stamp_account_search_contacts_finish (StampAccount  *self,
                                      EBookClient   *client,
                                      GAsyncResult  *res,
                                      GError       **error)
{
  g_return_val_if_fail (g_async_result_is_tagged (res, stamp_account_search_contacts), NULL);

  return g_task_propagate_pointer (G_TASK (res), error);
}

static void
scan_folder_for_contacts (CamelFolder  *folder,
                          const gchar  *lower_search,
                          GHashTable   *unique,
                          GCancellable *cancellable)
{
  g_autoptr (GPtrArray) uids = camel_folder_dup_uids (folder);
  CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);

  if (!uids || !summary)
    return;

  for (guint i = 0; i < uids->len; i++) {
    const gchar *uid;
    const gchar *from;
    g_autoptr (CamelMessageInfo) info = NULL;

    if (g_cancellable_is_cancelled (cancellable))
      break;

    uid = g_ptr_array_index (uids, i);
    info = camel_folder_summary_get (summary, uid);
    if (!info)
      continue;

    from = camel_message_info_get_from (info);
    if (from) {
      g_autoptr (CamelInternetAddress) addr = camel_internet_address_new ();

      if (camel_address_decode (CAMEL_ADDRESS (addr), from) > 0) {
        const gchar *name;
        const gchar *email;

        if (camel_internet_address_get (addr, 0, &name, &email) && email) {
          g_autofree char *lower_email = g_ascii_strdown (email, -1);
          gboolean matches = g_strstr_len (lower_email, -1, lower_search) != NULL;

          if (!matches && name) {
            g_autofree char *lower_name = g_ascii_strdown (name, -1);

            matches = g_strstr_len (lower_name, -1, lower_search) != NULL;
          }

          if (matches && !g_hash_table_contains (unique, lower_email)) {
            EContact *contact = e_contact_new ();
            GSList email_list;

            email_list.data = (gpointer)lower_email;
            email_list.next = NULL;

            e_contact_set (contact, E_CONTACT_FULL_NAME, name ? name : lower_email);
            e_contact_set (contact, E_CONTACT_EMAIL, &email_list);

            g_hash_table_insert (unique, g_strdup (lower_email), contact);
          }
        }
      }
    }
  }
}

static void
search_conversations_thread (GTask        *task,
                             gpointer      source_object,
                             gpointer      task_data,
                             GCancellable *cancellable)
{
  StampAccount *self = STAMP_ACCOUNT (source_object);
  const gchar *search_text = task_data;
  g_autoptr (GHashTable) unique = NULL;
  g_autoslist (EContact) results = NULL;
  GHashTableIter iter;
  gpointer value;

  unique = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  if (self->mail) {
    if (self->mail->sent_folder)
      scan_folder_for_contacts (self->mail->sent_folder, search_text, unique, cancellable);

    if (self->mail->service && !g_cancellable_is_cancelled (cancellable)) {
      g_autoptr (GError) error = NULL;
      CamelFolder *inbox;

      inbox = camel_store_get_folder_sync (CAMEL_STORE (self->mail->service),
                                           "INBOX", CAMEL_STORE_FOLDER_NONE,
                                           cancellable, &error);
      if (inbox) {
        scan_folder_for_contacts (inbox, search_text, unique, cancellable);
        g_object_unref (inbox);
      }
    }
  }

  g_hash_table_iter_init (&iter, unique);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    EContact *contact = value;

    results = g_slist_prepend (results, g_object_ref (contact));
  }

  if (g_cancellable_is_cancelled (cancellable)) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Search cancelled");
  } else {
    g_task_return_pointer (task, g_steal_pointer (&results), free_contact_slist);
  }
}

void
stamp_account_search_conversations (StampAccount        *self,
                                    const gchar         *search_text,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;
  gchar *lower_search;

  lower_search = g_ascii_strdown (search_text, -1);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, stamp_account_search_conversations);
  g_task_set_task_data (task, lower_search, g_free);
  g_task_run_in_thread (task, search_conversations_thread);
}

GSList *
stamp_account_search_conversations_finish (StampAccount  *self,
                                           GAsyncResult  *res,
                                           GError       **error)
{
  return g_task_propagate_pointer (G_TASK (res), error);
}

GPtrArray *
stamp_account_get_books (StampAccount *self)
{
  return self->address_books;
}

void
stamp_account_add_mail (StampAccount *self,
                        ESource      *source)
{
  if (!self->mail)
    self->mail = g_new0 (StampMailService, 1);
  self->mail->source = g_object_ref (source);
  self->mail->registry = self->registry;
}

void
stamp_account_add_address_book (StampAccount *self,
                                ESource      *source)
{
  StampContactsService *contacts = g_new0 (StampContactsService, 1);

  contacts->source = g_object_ref (source);
  contacts->enabled = FALSE;
  g_ptr_array_add (self->address_books, contacts);
}

static gboolean
is_goa_disabled (StampAccount *self,
                 ESource      *source)
{
  g_autoptr (ESource) goa_source = NULL;
  gboolean goa_mail_disabled = FALSE;

  goa_source = e_source_registry_find_extension (self->registry, source, E_SOURCE_EXTENSION_GOA);
  if (goa_source)
    goa_mail_disabled = !e_source_get_enabled (source);

  return goa_mail_disabled;
}

static StampContactsService *
find_book_by_source (StampAccount *self,
                     ESource      *source)
{
  if (!self->address_books)
    return NULL;

  for (guint idx = 0; idx < self->address_books->len; idx++) {
    StampContactsService *service = g_ptr_array_index (self->address_books, idx);

    if (service->source == source || g_strcmp0 (e_source_get_uid (service->source), e_source_get_uid (source)) == 0)
      return service;
  }

  return NULL;
}

static gchar *
address_book_source_get_string (ESource     *source,
                                const gchar *group,
                                const gchar *key)
{
  g_autofree gchar *data = e_source_to_string (source, NULL);
  g_autoptr (GKeyFile) key_file = g_key_file_new ();

  if (!g_key_file_load_from_data (key_file, data, -1, G_KEY_FILE_NONE, NULL))
    return NULL;

  return g_key_file_get_string (key_file, group, key, NULL);
}

static const gchar *
address_book_get_kind (StampAccount *self,
                       EClient      *client)
{
  ESource *source = e_client_get_source (client);
  ESourceAddressBook *extension = e_source_get_extension (source, E_SOURCE_EXTENSION_ADDRESS_BOOK);
  const gchar *backend = e_source_backend_get_backend_name (E_SOURCE_BACKEND (extension));

  if (g_strcmp0 (backend, "local") == 0)
    return "local";

  if (g_strcmp0 (backend, "microsoft365") == 0) {
    g_autofree gchar *folder_id = address_book_source_get_string (source, "Microsoft365 Folder", "Id");

    if (folder_id && g_ascii_strcasecmp (folder_id, "folder-id::users") == 0)
      return "organizational-users";
    if (folder_id && g_ascii_strcasecmp (folder_id, "folder-id::orgContacts") == 0)
      return "organizational-contacts";
  } else if (g_strcmp0 (backend, "ews") == 0 && self->registry) {
    const gchar *extension_name = e_source_camel_get_extension_name ("ews");
    g_autoptr (ESource) collection = e_source_registry_find_extension (self->registry, source, extension_name);

    if (collection) {
      g_autofree gchar *gal_uid = address_book_source_get_string (collection, extension_name, "GalUid");

      if (g_strcmp0 (gal_uid, e_source_get_uid (source)) == 0)
        return "global-address-list";
    }
  }

  return "contacts";
}

static gboolean
address_book_kind_is_directory (const gchar *kind)
{
  return g_str_equal (kind, "organizational-users") ||
         g_str_equal (kind, "organizational-contacts") ||
         g_str_equal (kind, "global-address-list");
}

gboolean
stamp_account_address_book_is_search_only (StampAccount *self,
                                           EClient      *client)
{
  return address_book_kind_is_directory (address_book_get_kind (self, client)) ||
         !e_client_check_capability (client, "do-initial-query");
}

static void
on_book_client_ready (GObject      *src,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  InitContext *ctx = user_data;
  g_autoptr (GError) error = NULL;
  EClient *client = e_book_client_connect_finish (res, &error);

  if (error) {
    g_warning ("%s: EBookClient failed: %s", G_STRFUNC, error->message);
    init_context_set_error (ctx, g_steal_pointer (&error));
  } else {
    ESource *source = e_client_get_source (client);
    StampContactsService *service = find_book_by_source (ctx->account, source);

    if (service) {
      service->client = E_BOOK_CLIENT (client);
      service->enabled = TRUE;
      g_debug ("%s: Book '%s' connected", G_STRFUNC, e_source_get_display_name (service->source));

      /* TODO: Currently we are deleting everything once a book is connected due to the fact
       * that books are loaded async. Therefore it might have happen that an existing avatar
       * is marked as negative. Check for a better alternative.
       */
      stamp_disk_cache_purge_negative (ctx->account->photo_cache);

      g_signal_emit (ctx->account, signals[BOOK_ADDED], 0, ctx->account, service);
    } else {
      g_clear_object (&client);
    }
  }

  init_context_finish_one (ctx);
}

typedef struct {
  StampAccount *account;
  InitContext *ctx;
  CamelStore *store;
  GCancellable *cancellable;
  GCancellable *parent_cancellable;
  gulong cancelled_id;
  gint pending;
  guint have_sent : 1;
  guint have_drafts : 1;
  guint have_trash : 1;
  guint have_junk : 1;
  guint have_archives : 1;
  guint have_templates : 1;
} MailEnableData;

static gboolean
mail_enable_is_current (MailEnableData *data)
{
  return !g_cancellable_is_cancelled (data->cancellable) &&
         data->account->mail && data->account->mail->enabled &&
         data->account->mail->service == CAMEL_SERVICE (data->store);
}

static void
on_mail_enable_cancelled (GCancellable *cancellable,
                          gpointer      user_data)
{
  g_cancellable_cancel (G_CANCELLABLE (user_data));
}

static void
mail_enable_one_done (MailEnableData *data)
{
  if (--data->pending > 0)
    return;

  if (mail_enable_is_current (data))
    g_signal_emit (data->account, signals[MAIL_ADDED], 0, data->account, data->account->mail->service);

  if (data->ctx)
    init_context_finish_one (data->ctx);

  g_cancellable_disconnect (data->parent_cancellable, data->cancelled_id);
  g_clear_object (&data->parent_cancellable);
  g_clear_object (&data->account);
  g_clear_object (&data->store);
  g_clear_object (&data->cancellable);
  g_free (data);
}

static void
on_junk_folder_ready (GObject      *src,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  MailEnableData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelFolder) folder = camel_store_get_folder_finish (CAMEL_STORE (src), res, &error);

  if (folder && mail_enable_is_current (data))
    data->account->mail->junk_folder = g_steal_pointer (&folder);
  else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("%s: %s", G_STRFUNC, error ? error->message : "");

  mail_enable_one_done (data);
}

static void
on_trash_folder_ready (GObject      *src,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  MailEnableData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelFolder) folder = camel_store_get_folder_finish (CAMEL_STORE (src), res, &error);

  if (folder && mail_enable_is_current (data))
    data->account->mail->trash_folder = g_steal_pointer (&folder);
  else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("%s: %s", G_STRFUNC, error ? error->message : "");

  mail_enable_one_done (data);
}

static void
on_sent_folder_ready (GObject      *src,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  MailEnableData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelFolder) folder = camel_store_get_folder_finish (CAMEL_STORE (src), res, &error);

  if (folder && mail_enable_is_current (data))
    data->account->mail->sent_folder = g_steal_pointer (&folder);
  else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("%s: %s", G_STRFUNC, error ? error->message : "");

  mail_enable_one_done (data);
}

static void
on_drafts_folder_ready (GObject      *src,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  MailEnableData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelFolder) folder = camel_store_get_folder_finish (CAMEL_STORE (src), res, &error);

  if (folder && mail_enable_is_current (data))
    data->account->mail->drafts_folder = g_steal_pointer (&folder);
  else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("%s: %s", G_STRFUNC, error ? error->message : "");

  mail_enable_one_done (data);
}

static void
on_archives_folder_ready (GObject      *src,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  MailEnableData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelFolder) folder = camel_store_get_folder_finish (CAMEL_STORE (src), res, &error);

  if (folder && mail_enable_is_current (data))
    data->account->mail->archives_folder = g_steal_pointer (&folder);
  else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("%s: %s", G_STRFUNC, error ? error->message : "");

  mail_enable_one_done (data);
}

static void
on_templates_folder_ready (GObject      *src,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  MailEnableData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelFolder) folder = camel_store_get_folder_finish (CAMEL_STORE (src), res, &error);

  if (folder && mail_enable_is_current (data))
    data->account->mail->templates_folder = g_steal_pointer (&folder);
  else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("%s: %s", G_STRFUNC, error ? error->message : "");

  mail_enable_one_done (data);
}

static void
on_folder_info_for_special_folders (GObject      *src,
                                    GAsyncResult *res,
                                    gpointer      user_data)
{
  MailEnableData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelFolderInfo) root = camel_store_get_folder_info_finish (CAMEL_STORE (src), res, &error);
  CamelFolderInfo *fi;
  g_autofree gchar *possible_sent = NULL;
  g_autofree gchar *possible_archives = NULL;
  g_autofree gchar *possible_drafts = NULL;
  g_autofree gchar *possible_trash = NULL;
  g_autofree gchar *possible_junk = NULL;
  g_autofree gchar *possible_templates = NULL;

  if (error || !mail_enable_is_current (data)) {
    if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("%s: get_folder_info: %s", G_STRFUNC, error->message);
    mail_enable_one_done (data);
    return;
  }

  fi = root;
  while (fi) {
    g_debug ("%s: Checking %s", G_STRFUNC, fi->display_name);

    if (!data->have_sent && ((fi->flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_SENT)) {
      data->have_sent = TRUE;
      data->pending += 1;
      camel_store_get_folder (CAMEL_STORE (src), fi->full_name, 0, G_PRIORITY_DEFAULT, data->cancellable, on_sent_folder_ready, data);
    } else if (!data->have_drafts && ((fi->flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_DRAFTS)) {
      data->have_drafts = TRUE;
      data->pending += 1;
      camel_store_get_folder (CAMEL_STORE (src), fi->full_name, 0, G_PRIORITY_DEFAULT, data->cancellable, on_drafts_folder_ready, data);
    } else if (!data->have_trash && ((fi->flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_TRASH)) {
      data->have_trash = TRUE;
      data->pending += 1;
      camel_store_get_folder (CAMEL_STORE (src), fi->full_name, 0, G_PRIORITY_DEFAULT, data->cancellable, on_trash_folder_ready, data);
    } else if (!data->have_junk && ((fi->flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_JUNK)) {
      data->have_junk = TRUE;
      data->pending += 1;
      camel_store_get_folder (CAMEL_STORE (src), fi->full_name, 0, G_PRIORITY_DEFAULT, data->cancellable, on_junk_folder_ready, data);
    } else if (!data->have_archives && ((fi->flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_ARCHIVE)) {
      data->have_archives = TRUE;
      data->pending += 1;
      camel_store_get_folder (CAMEL_STORE (src), fi->full_name, 0, G_PRIORITY_DEFAULT, data->cancellable, on_archives_folder_ready, data);
    } else if (!data->have_templates && g_strcmp0 (fi->display_name, "Templates") == 0) {
      data->have_templates = TRUE;
      data->pending += 1;
      camel_store_get_folder (CAMEL_STORE (src), fi->full_name, 0, G_PRIORITY_DEFAULT, data->cancellable, on_templates_folder_ready, data);
    } else {
      /* Ugly part, let's try to guess based on it's name */

      if (!data->have_sent &&
          (g_ascii_strcasecmp (fi->display_name, "Sent") == 0 ||
           g_ascii_strcasecmp (fi->display_name, "Sent Mail") == 0 ||
           g_ascii_strcasecmp (fi->display_name, "Gesendet") == 0)) {
        g_set_str (&possible_sent, fi->full_name);
      } else if (!data->have_archives &&
                 (g_ascii_strcasecmp (fi->display_name, "Archive") == 0 ||
                  g_ascii_strcasecmp (fi->display_name, "Archiv") == 0 ||  /* codespell:ignore */
                  g_ascii_strcasecmp (fi->display_name, "All Mail") == 0 ||
                  g_ascii_strcasecmp (fi->display_name, "Alle Nachrichten") == 0)) { /* codespell:ignore */
        g_set_str (&possible_archives, fi->full_name);
      } else if (!data->have_drafts &&
                 (g_ascii_strcasecmp (fi->display_name, "Drafts") == 0 ||
                  g_ascii_strcasecmp (fi->display_name, "Entwürfe") == 0)) {
        g_set_str (&possible_drafts, fi->full_name);
      } else if (!data->have_trash &&
                 (g_ascii_strcasecmp (fi->display_name, "Trash") == 0 ||
                  g_ascii_strcasecmp (fi->display_name, "Papierkorb") == 0)) {
        g_set_str (&possible_trash, fi->full_name);
      } else if (!data->have_junk &&
                 (g_ascii_strcasecmp (fi->display_name, "Junk") == 0 ||
                  g_ascii_strcasecmp (fi->display_name, "Spam") == 0)) {
        g_set_str (&possible_junk, fi->full_name);
      } else if (!data->have_templates &&
                 (g_ascii_strcasecmp (fi->display_name, "Templates") == 0 ||
                  g_ascii_strcasecmp (fi->display_name, "Vorlagen") == 0)) {
        g_set_str (&possible_templates, fi->full_name);
      }
    }

    if (fi->child) {
      fi = fi->child;
      continue;
    }

    while (fi && !fi->next)
      fi = fi->parent;

    if (fi)
      fi = fi->next;
  }

  if (!data->have_sent && possible_sent) {
    data->have_sent = TRUE;
    data->pending += 1;

    g_debug ("%s: Trying to load fallback sent folder", G_STRFUNC);
    camel_store_get_folder (CAMEL_STORE (src), possible_sent, 0, G_PRIORITY_DEFAULT, data->cancellable, on_sent_folder_ready, data);
  }

  if (!data->have_archives && possible_archives) {
    data->have_archives = TRUE;
    data->pending += 1;

    g_debug ("%s: Trying to load fallback archives folder", G_STRFUNC);
    camel_store_get_folder (CAMEL_STORE (src), possible_archives, 0, G_PRIORITY_DEFAULT, data->cancellable, on_archives_folder_ready, data);
  }

  if (!data->have_drafts && possible_drafts) {
    data->have_drafts = TRUE;
    data->pending += 1;

    g_debug ("%s: Trying to load fallback drafts folder", G_STRFUNC);
    camel_store_get_folder (CAMEL_STORE (src), possible_drafts, 0, G_PRIORITY_DEFAULT, data->cancellable, on_drafts_folder_ready, data);
  }

  if (!data->have_trash && possible_trash) {
    data->have_trash = TRUE;
    data->pending += 1;

    g_debug ("%s: Trying to load fallback trash folder", G_STRFUNC);
    camel_store_get_folder (CAMEL_STORE (src), possible_trash, 0, G_PRIORITY_DEFAULT, data->cancellable, on_trash_folder_ready, data);
  }

  if (!data->have_junk && possible_junk) {
    data->have_junk = TRUE;
    data->pending += 1;

    g_debug ("%s: Trying to load fallback junk folder", G_STRFUNC);
    camel_store_get_folder (CAMEL_STORE (src), possible_junk, 0, G_PRIORITY_DEFAULT, data->cancellable, on_junk_folder_ready, data);
  }

  if (!data->have_templates && possible_templates) {
    data->have_templates = TRUE;
    data->pending += 1;

    g_debug ("%s: Trying to load fallback templates folder", G_STRFUNC);
    camel_store_get_folder (CAMEL_STORE (src), possible_templates, 0, G_PRIORITY_DEFAULT, data->cancellable, on_templates_folder_ready, data);
  }

  mail_enable_one_done (data);
}

static void
on_initial_setup_ready (GObject      *src,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  MailEnableData *data = user_data;
  StampAccount *self = data->account;
  g_autoptr (GHashTable) setup = NULL;
  g_autoptr (GError) setup_error = NULL;
  gboolean setup_success;
  const gchar *archives_path = NULL;
  const gchar *templates_path = NULL;
  const gchar *spam_path = NULL;
  const gchar *sent_path = NULL;
  const gchar *drafts_path = NULL;
  const gchar *trash_path = NULL;

  setup_success = camel_store_initial_setup_finish (CAMEL_STORE (src), res, &setup, &setup_error);
  if (!mail_enable_is_current (data)) {
    mail_enable_one_done (data);
    return;
  }

  if (setup_success) {
    if (setup) {
      archives_path = g_hash_table_lookup (setup, CAMEL_STORE_SETUP_ARCHIVE_FOLDER);
      drafts_path = g_hash_table_lookup (setup, CAMEL_STORE_SETUP_DRAFTS_FOLDER);
      sent_path = g_hash_table_lookup (setup, CAMEL_STORE_SETUP_SENT_FOLDER);
      spam_path = g_hash_table_lookup (setup, "Backend:Imapx Backend:real-junk-path:s");
      templates_path = g_hash_table_lookup (setup, CAMEL_STORE_SETUP_TEMPLATES_FOLDER);
      trash_path = g_hash_table_lookup (setup, "Backend:Imapx Backend:real-trash-path:s");

      g_debug ("%s: '%s' paths:", G_STRFUNC, stamp_account_get_name (self));
      g_debug ("%s: \tArchive: %s", G_STRFUNC, archives_path);
      g_debug ("%s: \tDrafts: %s", G_STRFUNC, drafts_path);
      g_debug ("%s: \tSent: %s", G_STRFUNC, sent_path);
      g_debug ("%s: \tSpam: %s", G_STRFUNC, spam_path);
      g_debug ("%s: \tTemplates: %s", G_STRFUNC, templates_path);
      g_debug ("%s: \tTrash: %s", G_STRFUNC, trash_path);
    }
  }

  if (setup_error) {
    g_warning ("%s: Could not query server defaults: %s", G_STRFUNC, setup_error->message);
  }

  data->have_sent = sent_path != NULL;
  data->have_drafts = drafts_path != NULL;
  data->have_trash = trash_path != NULL;
  data->have_junk = spam_path != NULL;
  data->have_archives = archives_path != NULL;
  data->have_templates = templates_path != NULL;

  /* In an ideal world camel would have send us all required folders, but there are some special cases like Google
   * which should be fixed in e-d-s...
   *
   * Would love to have functions like:
   *   - camel_store_get_[sent|drafts|... ]_folder
   */
  if (sent_path) {
    data->pending += 1;
    camel_store_get_folder (data->store, sent_path, 0, G_PRIORITY_DEFAULT, data->cancellable, on_sent_folder_ready, data);
  }

  if (drafts_path) {
    data->pending += 1;
    camel_store_get_folder (data->store, drafts_path, 0, G_PRIORITY_DEFAULT, data->cancellable, on_drafts_folder_ready, data);
  }

  if (trash_path) {
    data->pending += 1;
    camel_store_get_folder (data->store, trash_path, 0, G_PRIORITY_DEFAULT, data->cancellable, on_trash_folder_ready, data);
  }

  if (spam_path) {
    data->pending += 1;
    camel_store_get_folder (data->store, spam_path, 0, G_PRIORITY_DEFAULT, data->cancellable, on_junk_folder_ready, data);
  }

  if (archives_path) {
    data->pending += 1;
    camel_store_get_folder (data->store, archives_path, 0, G_PRIORITY_DEFAULT, data->cancellable, on_archives_folder_ready, data);
  }

  if (templates_path) {
    data->pending += 1;
    camel_store_get_folder (data->store, templates_path, 0, G_PRIORITY_DEFAULT, data->cancellable, on_templates_folder_ready, data);
  }

  if (!sent_path || !drafts_path || !trash_path || !spam_path || !archives_path || !templates_path) {
    data->pending += 1;

    /* We do not have all path we want, let's do it the hard way and traverse folder info */
    camel_store_get_folder_info (data->store, NULL, CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_NO_VIRTUAL, G_PRIORITY_DEFAULT, data->cancellable, on_folder_info_for_special_folders, data);
  }

  mail_enable_one_done (data);
}

static void
on_categories_ready (GObject      *src,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  MailEnableData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autolist (StampCategory) categories = stamp_m365_get_categories_finish (E_SOURCE (src), res, &error);

  if (mail_enable_is_current (data) && !error) {
    g_list_free_full (data->account->categories, g_object_unref);
    data->account->categories = g_steal_pointer (&categories);
  } else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_warning ("%s: Categories: %s", G_STRFUNC, error->message);
  }
  mail_enable_one_done (data);
}

static void
stamp_account_enable_mail_async (StampAccount *self,
                                 InitContext  *ctx)
{
  MailEnableData *data;
  g_autoptr (GError) error = NULL;
  ESourceMailAccount *extension;
  ESourceMailTransport *transport_extension;
  GCancellable *cancellable;

  g_cancellable_cancel (self->mail_cancellable);
  g_clear_object (&self->mail_cancellable);
  self->mail_cancellable = g_cancellable_new ();
  self->mail->enabled = TRUE;
  extension = e_source_get_extension (self->mail->source, E_SOURCE_EXTENSION_MAIL_ACCOUNT);

  self->mail->service = camel_session_add_service (CAMEL_SESSION (stamp_session_get_default ()),
                                                   e_source_get_uid (self->mail->source),
                                                   e_source_backend_get_backend_name (E_SOURCE_BACKEND (extension)),
                                                   CAMEL_PROVIDER_STORE,
                                                   &error);
  if (!self->mail->service) {
    g_warning ("%s: Failed to add mail service of type '%s' (is the matching Camel provider, e.g. evolution-ews for 'microsoft365', installed and visible via CAMEL_PROVIDERDIR?): %s",
               G_STRFUNC,
               e_source_backend_get_backend_name (E_SOURCE_BACKEND (extension)),
               error ? error->message : "unknown error");
    g_clear_error (&error);
    if (ctx)
      init_context_finish_one (ctx);
    return;
  }

  transport_extension = e_source_get_extension (self->mail->transport_source, E_SOURCE_EXTENSION_MAIL_TRANSPORT);
  self->mail->transport = CAMEL_TRANSPORT (camel_session_add_service (CAMEL_SESSION (stamp_session_get_default ()),
                                                                      e_source_get_uid (self->mail->transport_source),
                                                                      e_source_backend_get_backend_name (E_SOURCE_BACKEND (transport_extension)),
                                                                      CAMEL_PROVIDER_TRANSPORT, &error));
  if (!self->mail->transport)
    g_warning ("%s: Failed to add transport: %s", G_STRFUNC, error ? error->message : "");

  data = g_new0 (MailEnableData, 1);
  data->account = g_object_ref (self);
  data->ctx = ctx;
  data->store = g_object_ref (CAMEL_STORE (self->mail->service));
  data->cancellable = g_object_ref (self->mail_cancellable);
  cancellable = ctx ? g_task_get_cancellable (ctx->parent_task) : self->cancellable;
  if (cancellable) {
    data->parent_cancellable = g_object_ref (cancellable);
    data->cancelled_id = g_cancellable_connect (cancellable,
                                                G_CALLBACK (on_mail_enable_cancelled),
                                                g_object_ref (data->cancellable),
                                                g_object_unref);
  }
  /* Keep setup counted until all dependent folder requests have been queued. */
  data->pending = 1;

  g_debug ("%s: Query '%s' store…", G_STRFUNC, stamp_account_get_name (self));
  camel_store_initial_setup (data->store, G_PRIORITY_DEFAULT, data->cancellable, on_initial_setup_ready, data);

  if (g_strcmp0 (e_source_backend_get_backend_name (E_SOURCE_BACKEND (extension)), "microsoft365") == 0) {
    data->pending++;
    stamp_m365_get_categories_async (self->collection, self->mail->service, data->cancellable, on_categories_ready, data);
  }
}

void
stamp_account_init_async (StampAccount        *self,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  gboolean mail_enabled = self->mail && e_source_get_enabled (self->mail->source);
  InitContext *ctx;
  guint pending = 0;

  g_task_set_source_tag (task, stamp_account_init_async);
  g_task_set_task_data (task, self, NULL);

  if (mail_enabled)
    pending++;

  pending += self->address_books->len;

  if (pending == 0) {
    g_debug ("%s: No active services (%s)", G_STRFUNC, self->display_name);
    g_task_return_boolean (task, TRUE);
    return;
  }
  g_debug ("%s: %d pending", G_STRFUNC, pending);

  ctx = g_new0 (InitContext, 1);
  ctx->account = self;
  ctx->parent_task = g_object_ref (task);
  ctx->pending = pending + 1;
  g_mutex_init (&ctx->lock);

  if (mail_enabled)
    stamp_account_enable_mail_async (self, ctx);

  for (guint i = 0; i < self->address_books->len; i++) {
    StampContactsService *service = g_ptr_array_index (self->address_books, i);
    gboolean enabled;

    enabled = !is_goa_disabled (self, service->source);
    if (!enabled) {
      g_debug ("%s: Address book '%s' in account '%s' disabled", G_STRFUNC, e_source_get_display_name (service->source), self->display_name);
      service->enabled = FALSE;
      g_mutex_lock (&ctx->lock);
      ctx->pending--;
      g_mutex_unlock (&ctx->lock);
      continue;
    }
    g_debug ("%s: Address book '%s' in account '%s' enabled", G_STRFUNC, e_source_get_display_name (service->source), self->display_name);

    e_book_client_connect (service->source, 10, cancellable, on_book_client_ready, ctx);
  }

  init_context_finish_one (ctx);
}

gboolean
stamp_account_init_finish (GAsyncResult  *res,
                           GError       **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}

static
StampContactsService *
find_contacts_by_source (StampAccount *self,
                         ESource      *source)
{
  for (guint i = 0; i < self->address_books->len; i++) {
    StampContactsService *book = g_ptr_array_index (self->address_books, i);

    if (book->source == source)
      return book;
  }

  return NULL;
}

static void
on_book_client_connect (GObject      *src,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  StampAccount *self = STAMP_ACCOUNT (user_data);
  StampContactsService *service;
  ESource *source;
  g_autoptr (GError) error = NULL;
  g_autoptr (EClient) client = e_book_client_connect_finish (res, &error);

  if (error) {
    g_warning ("%s: EBookClient failed: %s", G_STRFUNC, error->message);
    return;
  }

  source = e_client_get_source (client);
  service = find_book_by_source (self, source);
  if (service) {
    service->client = E_BOOK_CLIENT (g_steal_pointer (&client));
    service->enabled = TRUE;
    g_debug ("%s: Book '%s' connected", G_STRFUNC, e_source_get_display_name (service->source));

    stamp_disk_cache_purge_negative (self->photo_cache);
    g_signal_emit (self, signals[BOOK_ADDED], 0, self, service);
  }
}

void
stamp_account_contacts_changed (StampAccount *self,
                                ESource      *source)
{
  StampContactsService *cal = find_contacts_by_source (self, source);
  gboolean enabled = e_source_get_enabled (source);

  if (!cal)
    return;

  if (!enabled && cal->client) {
    g_debug ("%s: Address book '%s' from '%s' disabled", G_STRFUNC, e_source_get_display_name (source), self->display_name);
    g_signal_emit (self, signals[BOOK_REMOVED], 0, self, cal);
    e_client_cancel_all (E_CLIENT (cal->client));
    g_clear_object (&cal->client);
    cal->enabled = FALSE;
  } else if (enabled && !cal->client) {
    g_debug ("%s: Address book '%s' from '%s' enabled", G_STRFUNC, e_source_get_display_name (source), self->display_name);

    e_book_client_connect (source, 10, NULL, on_book_client_connect, self);
  }
}

void
stamp_account_mail_changed (StampAccount *self,
                            ESource      *source)
{
  gboolean enabled = e_source_get_enabled (source);

  if (!enabled) {
    g_debug ("%s: Mail '%s' from '%s' disabled", G_STRFUNC, e_source_get_display_name (source), self->display_name);
    g_cancellable_cancel (self->mail_cancellable);
    g_clear_object (&self->mail_cancellable);
    self->mail->enabled = FALSE;
    g_signal_emit (self, signals[MAIL_REMOVED], 0, self, self->mail);
    camel_service_disconnect_sync (self->mail->service, TRUE, NULL, NULL);
    g_clear_object (&self->mail->service);
    g_clear_object (&self->mail->transport);
  } else {
    g_debug ("%s: Mail '%s' from '%s' enabled", G_STRFUNC, e_source_get_display_name (source), self->display_name);

    stamp_account_enable_mail_async (self, NULL);
  }
}

void
stamp_account_set_name (StampAccount *self,
                        const gchar  *name)
{
  if (!name || self->display_name == name)
    return;

  g_set_str (&self->display_name, name);
  update_all_accounts_colors ();
}

void
stamp_account_add_mail_transport (StampAccount *self,
                                  ESource      *source)
{
  if (!self->mail)
    self->mail = g_new0 (StampMailService, 1);
  self->mail->transport_source = g_object_ref (source);
}

void
stamp_account_add_mail_identity (StampAccount *self,
                                 ESource      *source)
{
  ESourceMailIdentity *identity = e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_IDENTITY);
  const gchar *name = e_source_mail_identity_get_name (identity);
  const gchar *address = e_source_mail_identity_get_address (identity);
  GHashTable *aliases = e_source_mail_identity_get_aliases_as_hash_table (identity);

  g_debug ("%s: Setting own address to name %s, address %s, aliases %p", G_STRFUNC, name, address, aliases);
  if (!self->mail)
    self->mail = g_new0 (StampMailService, 1);

  self->mail->address = camel_internet_address_new ();
  camel_internet_address_add (self->mail->address, name, address);

  self->mail->aliases = aliases;
  self->mail->identity_source = g_object_ref (source);
}

static gchar *
alias_to_string (GHashTable *aliases)
{
  GString *str = g_string_new ("");
  GList *keys = g_hash_table_get_keys (aliases);
  GList *iter;

  for (iter = keys; iter; iter = g_list_next (iter)) {
    const gchar *mail = iter->data;
    const gchar *name = g_hash_table_lookup (aliases, mail);
    g_autofree char *encoded = NULL;

    encoded = camel_internet_address_encode_address (NULL, name, mail);
    if (encoded && *encoded) {
      if (str->len > 0)
        g_string_append (str, ",");

      g_string_append (str, encoded);
    }
  }

  g_list_free (keys);

  return g_string_free_and_steal (str);
}

void
stamp_mail_service_set_aliases (StampMailService *self,
                                GHashTable       *aliases)
{
  ESource *source;
  ESourceMailIdentity *identity;
  g_autofree char *alias_str = NULL;
  g_autoptr (GError) error = NULL;

  source = self->identity_source;
  if (!source)
    return;

  identity = e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_IDENTITY);
  alias_str = alias_to_string (aliases);
  e_source_mail_identity_set_aliases (identity, alias_str);

  e_source_registry_commit_source_sync (self->registry, source, NULL, &error);
  if (error)
    g_warning ("Could not commit alias: %s", error->message);

  self->aliases = e_source_mail_identity_get_aliases_as_hash_table (identity);
}

CamelInternetAddress *
stamp_account_get_address (StampAccount *self)
{
  if (!self->mail)
    return NULL;

  return self->mail->address;
}

static void
stamp_account_append_to_sent_folder (StampAccount      *self,
                                     gboolean           server_saved,
                                     CamelMimeMessage  *message,
                                     GCancellable      *cancellable,
                                     GError           **error)
{
  CamelFolder *sent_folder;
  g_autoptr (CamelMessageInfo) info = NULL;
  gboolean ok;

  if (server_saved)
    return;

  sent_folder = stamp_account_get_mail_sent_folder (self);
  if (!sent_folder) {
    g_warning ("%s: No sent folder, abort", G_STRFUNC);
    return;
  }

  info = camel_message_info_new (NULL);
  camel_message_info_set_flags (info, CAMEL_MESSAGE_SEEN, CAMEL_MESSAGE_SEEN);

  ok = camel_folder_append_message_sync (sent_folder, message, info, NULL, cancellable, error);
  if (ok)
    camel_folder_synchronize_sync (sent_folder, FALSE, cancellable, NULL);
}

gpointer
stamp_account_send_mail_finish (StampAccount  *account,
                                GAsyncResult  *result,
                                GError       **error)
{
  gpointer ptr;

  g_assert (g_task_is_valid (result, account));
  g_assert (error && !*error);

  ptr = g_task_propagate_pointer (G_TASK (result), error);

  return ptr;
}

typedef struct {
  CamelMimeMessage *message;
  CamelInternetAddress *sender;
  CamelInternetAddress *recipient;
  gboolean pgp_sign;
  gboolean pgp_encrypt;
} SendMailData;

static void
send_mail_data_free (gpointer user_data)
{
  SendMailData *data = user_data;

  g_clear_object (&data->message);
  g_clear_object (&data->sender);
  g_clear_object (&data->recipient);
  g_clear_pointer (&data, g_free);
}

static void
send_mail (GTask        *task,
           gpointer      source_object,
           gpointer      task_data,
           GCancellable *cancellable)
{
  StampAccount *self = STAMP_ACCOUNT (source_object);
  SendMailData *data = task_data;
  g_autoptr (GError) error = NULL;
  gboolean out_saved;

  if (!camel_service_connect_sync (CAMEL_SERVICE (self->mail->transport), cancellable, &error)) {
    g_warning ("%s: Could not connect to transport service '%s': %s", G_STRFUNC, e_source_get_display_name (self->mail->transport_source), error->message);
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  if (!camel_transport_send_to_sync (self->mail->transport, data->message, CAMEL_ADDRESS (data->sender), CAMEL_ADDRESS (data->recipient), &out_saved, cancellable, &error)) {
    g_warning ("%s: Could not send to transport service '%s': %s", G_STRFUNC, e_source_get_display_name (self->mail->transport_source), error->message);
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  stamp_account_append_to_sent_folder (self, out_saved, data->message, NULL, NULL);

  if (!camel_service_disconnect_sync (CAMEL_SERVICE (self->mail->transport), TRUE, cancellable, &error)) {
    g_warning ("%s: Could not disconnect from transport service '%s': %s", G_STRFUNC, e_source_get_display_name (self->mail->transport_source), error->message);
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  g_task_return_boolean (task, TRUE);
}

void
stamp_account_send_mail (StampAccount         *self,
                         CamelMimeMessage     *message,
                         CamelInternetAddress *sender,
                         CamelInternetAddress *recipient,
                         gboolean              pgp_sign,
                         gboolean              pgp_encrypt,
                         GCancellable         *cancellable,
                         GAsyncReadyCallback   callback,
                         gpointer              user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  SendMailData *data = g_new0 (SendMailData, 1);

  data->message = g_object_ref (message);
  data->sender = g_object_ref (sender);
  data->recipient = g_object_ref (recipient);
  data->pgp_sign = pgp_sign;
  data->pgp_encrypt = pgp_encrypt;

  g_task_set_source_tag (task, stamp_account_send_mail);
  g_task_set_task_data (task, data, send_mail_data_free);
  g_task_run_in_thread (task, send_mail);
}

StampMailService *
stamp_account_get_mail_service (StampAccount *self)
{
  return self->mail;
}

CamelService *
stamp_mail_service_get_service (StampMailService *self)
{
  return self->service;
}

CamelTransport *
stamp_mail_service_get_transport (StampMailService *self)
{
  return self->transport;
}

gboolean
stamp_mail_service_get_enabled (StampMailService *self)
{
  return self->enabled;
}

ESource *
stamp_mail_service_get_source (StampMailService *self)
{
  return self->source;
}

ESource *
stamp_mail_service_get_transport_source (StampMailService *self)
{
  return self->transport_source;
}

ESource *
stamp_mail_service_get_identity_source (StampMailService *self)
{
  return self->identity_source;
}

gboolean
stamp_contacts_service_get_enabled (StampContactsService *self)
{
  return self->enabled;
}

EBookClient *
stamp_contacts_service_get_client (StampContactsService *self)
{
  return self->client;
}

ESource *
stamp_contacts_service_get_source (StampContactsService *self)
{
  return self->source;
}

static void
on_folder_synchronized (GObject      *source,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  CamelFolder *folder = CAMEL_FOLDER (source);
  g_autoptr (GError) error = NULL;

  if (!camel_folder_synchronize_finish (folder, res, &error)) {
    g_warning ("%s: Could not synchronize folder %s: %s", G_STRFUNC, camel_folder_get_display_name (folder), error->message);
  }
}

gchar *
stamp_account_save_draft (StampAccount         *self,
                          const gchar          *draft_uid,
                          CamelMimeMessage     *message,
                          CamelInternetAddress *sender,
                          CamelInternetAddress *recipient)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelMessageInfo) info = camel_message_info_new (NULL);
  gchar *uid = NULL;

  if (!self->mail->drafts_folder) {
    g_warning ("%s: No drafts folder", G_STRFUNC);
    return NULL;
  }

  camel_message_info_set_flags (info, CAMEL_MESSAGE_DRAFT | CAMEL_MESSAGE_SEEN, CAMEL_MESSAGE_DRAFT | CAMEL_MESSAGE_SEEN);
  if (!camel_folder_append_message_sync (self->mail->drafts_folder, message, info, &uid, self->cancellable, &error)) {
    if (error)
      g_warning ("%s: Could not append message to drafts folder: %s", G_STRFUNC, error->message);

    return NULL;
  }

  if (draft_uid) {
    camel_folder_delete_message (self->mail->drafts_folder, draft_uid);
    camel_folder_synchronize (self->mail->drafts_folder, TRUE, G_PRIORITY_DEFAULT, self->cancellable, on_folder_synchronized, self);
  }

  return uid;
}

gboolean
stamp_account_save_template (StampAccount     *self,
                             CamelMimeMessage *message)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelMessageInfo) info = camel_message_info_new (NULL);

  if (!self->mail->templates_folder) {
    g_warning ("%s: No templates folder", G_STRFUNC);
    return FALSE;
  }

  camel_message_info_set_flags (info, CAMEL_MESSAGE_DRAFT | CAMEL_MESSAGE_SEEN, CAMEL_MESSAGE_DRAFT | CAMEL_MESSAGE_SEEN);
  if (!camel_folder_append_message_sync (self->mail->templates_folder, message, info, NULL, self->cancellable, &error)) {
    if (error)
      g_warning ("%s: Could not append message to templates folder: %s", G_STRFUNC, error->message);

    return FALSE;
  }

  return TRUE;
}

void
stamp_account_remove_draft (StampAccount *self,
                            const gchar  *uid)
{
  g_autoptr (CamelMessageInfo) info = NULL;
  g_autoptr (GError) error = NULL;

  if (!uid) {
    g_warning ("%s: Called with uid = NULL", G_STRFUNC);
    return;
  }

  camel_folder_set_message_flags (self->mail->drafts_folder, uid, CAMEL_MESSAGE_DRAFT | CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);
  camel_folder_synchronize (self->mail->drafts_folder, TRUE, G_PRIORITY_DEFAULT, self->cancellable, on_folder_synchronized, self);
}

typedef struct {
  StampAccount *account;
  gchar *draft_uid;
  CamelMimeMessage *message;
  CamelInternetAddress *sender;
  CamelInternetAddress *recipient;
} SaveDraftData;

static void
save_draft_data_free (gpointer user_data)
{
  SaveDraftData *data = user_data;

  g_clear_object (&data->account);
  g_clear_pointer (&data->draft_uid, g_free);
  g_clear_object (&data->message);
  g_clear_object (&data->sender);
  g_clear_object (&data->recipient);
  g_clear_pointer (&data, g_free);
}

static void
save_draft_thread (GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data,
                   GCancellable *cancellable)
{
  SaveDraftData *data = task_data;
  gchar *uid;

  uid = stamp_account_save_draft (data->account,
                                  data->draft_uid,
                                  data->message,
                                  data->sender,
                                  data->recipient);

  if (uid)
    g_task_return_pointer (task, uid, g_free);
  else
    g_task_return_pointer (task, NULL, NULL);
}

void
stamp_account_save_draft_async (StampAccount         *self,
                                const gchar          *draft_uid,
                                CamelMimeMessage     *message,
                                CamelInternetAddress *sender,
                                CamelInternetAddress *recipient,
                                GCancellable         *cancellable,
                                GAsyncReadyCallback   callback,
                                gpointer              user_data)
{
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  SaveDraftData *data = g_new0 (SaveDraftData, 1);

  data->account = g_object_ref (self);
  data->draft_uid = g_strdup (draft_uid);
  data->message = g_object_ref (message);
  data->sender = g_object_ref (sender);
  data->recipient = g_object_ref (recipient);

  g_task_set_source_tag (task, stamp_account_save_draft_async);
  g_task_set_task_data (task, data, save_draft_data_free);
  g_task_run_in_thread (task, save_draft_thread);
}

gchar *
stamp_account_save_draft_async_finish (StampAccount  *self,
                                       GAsyncResult  *result,
                                       GError       **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

typedef struct {
  StampAccount *account;
  gchar *uid;
} RemoveDraftData;

static void
remove_draft_data_free (gpointer user_data)
{
  RemoveDraftData *data = user_data;

  g_clear_object (&data->account);
  g_clear_pointer (&data->uid, g_free);
  g_clear_pointer (&data, g_free);
}

static void
remove_draft_thread (GTask        *task,
                     gpointer      source_object,
                     gpointer      task_data,
                     GCancellable *cancellable)
{
  RemoveDraftData *data = task_data;

  stamp_account_remove_draft (data->account, data->uid);

  g_task_return_boolean (task, TRUE);
}

void
stamp_account_remove_draft_async (StampAccount        *self,
                                  const gchar         *uid,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  RemoveDraftData *data = g_new0 (RemoveDraftData, 1);

  data->account = g_object_ref (self);
  data->uid = g_strdup (uid);

  g_task_set_source_tag (task, stamp_account_remove_draft_async);
  g_task_set_task_data (task, data, remove_draft_data_free);
  g_task_run_in_thread (task, remove_draft_thread);
}

gboolean
stamp_account_remove_draft_async_finish (StampAccount  *self,
                                         GAsyncResult  *result,
                                         GError       **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

CamelFolder *
stamp_account_get_mail_trash_folder (StampAccount *self)
{
  return self->mail->trash_folder;
}

CamelFolder *
stamp_account_get_mail_sent_folder (StampAccount *self)
{
  return self->mail->sent_folder;
}

CamelFolder *
stamp_account_get_mail_drafts_folder (StampAccount *self)
{
  return self->mail->drafts_folder;
}

void
stamp_account_clear_negative_photo_cache (StampAccount *self)
{
  stamp_disk_cache_purge_negative (self->photo_cache);
}

StampCategory *
stamp_account_find_category (StampAccount *self,
                             const gchar  *name)
{
  g_autoptr (GString) str = NULL;

  if (!self->categories)
    return NULL;

  str = g_string_new (name);
  g_string_replace (str, "_", " ", 0);

  for (GList *iter = self->categories; iter && iter->data; iter = g_list_next (iter)) {
    StampCategory *cat = iter->data;

    if (g_strcmp0 (stamp_category_get_name (cat), str->str) == 0)
      return g_object_ref (cat);
  }

  return NULL;
}

GList *
stamp_account_get_categories (StampAccount *self)
{
  return self->categories;
}

GHashTable *
stamp_mail_service_get_aliases (StampMailService *self)
{
  return self->aliases;
}

CamelFolder *
stamp_account_get_mail_junk_folder (StampAccount *self)
{
  return self->mail->junk_folder;
}

CamelFolder *
stamp_account_get_mail_archives_folder (StampAccount *self)
{
  return self->mail->archives_folder;
}

CamelFolder *
stamp_account_get_mail_templates_folder (StampAccount *self)
{
  return self->mail->templates_folder;
}

gint
stamp_account_get_folder_order_rank (StampAccount    *self,
                                     CamelFolderInfo *info)
{
  const gchar *full_name;
  guint flags;

  if (!info)
    return STAMP_FOLDER_TYPE_NORMAL;

  full_name = info->full_name ? info->full_name : info->display_name;

  if (self && self->mail) {
    if (self->mail->trash_folder && full_name && g_strcmp0 (camel_folder_get_full_name (self->mail->trash_folder), full_name) == 0)
      return STAMP_FOLDER_TYPE_TRASH;

    if (self->mail->drafts_folder && full_name && g_strcmp0 (camel_folder_get_full_name (self->mail->drafts_folder), full_name) == 0)
      return STAMP_FOLDER_TYPE_DRAFTS;

    if (self->mail->sent_folder && full_name && g_strcmp0 (camel_folder_get_full_name (self->mail->sent_folder), full_name) == 0)
      return STAMP_FOLDER_TYPE_SENT;

    if (self->mail->junk_folder && full_name && g_strcmp0 (camel_folder_get_full_name (self->mail->junk_folder), full_name) == 0)
      return STAMP_FOLDER_TYPE_JUNK;

    if (self->mail->archives_folder && full_name && g_strcmp0 (camel_folder_get_full_name (self->mail->archives_folder), full_name) == 0)
      return STAMP_FOLDER_TYPE_ARCHIVES;

    if (self->mail->templates_folder && full_name && g_strcmp0 (camel_folder_get_full_name (self->mail->templates_folder), full_name) == 0)
      return STAMP_FOLDER_TYPE_TEMPLATES;
  }

  flags = info->flags & CAMEL_FOLDER_TYPE_MASK;
  switch (flags) {
    case CAMEL_FOLDER_TYPE_INBOX:
      return STAMP_FOLDER_TYPE_INBOX;
    case CAMEL_FOLDER_TYPE_OUTBOX:
      return STAMP_FOLDER_TYPE_OUTBOX;
    case CAMEL_FOLDER_TYPE_TRASH:
      return STAMP_FOLDER_TYPE_TRASH;
    case CAMEL_FOLDER_TYPE_JUNK:
      return STAMP_FOLDER_TYPE_JUNK;
    case CAMEL_FOLDER_TYPE_SENT:
      return STAMP_FOLDER_TYPE_SENT;
    case CAMEL_FOLDER_TYPE_ARCHIVE:
      return STAMP_FOLDER_TYPE_ARCHIVES;
    case CAMEL_FOLDER_TYPE_DRAFTS:
      return STAMP_FOLDER_TYPE_DRAFTS;
    default:
      break;
  }

  if (g_ascii_strcasecmp (info->display_name, "Starred") == 0 ||
      g_ascii_strcasecmp (info->display_name, "Markiert") == 0)
    return STAMP_FOLDER_TYPE_STARRED;

  if (g_ascii_strcasecmp (info->display_name, "Important") == 0 ||
      g_ascii_strcasecmp (info->display_name, "Wichtig") == 0)
    return STAMP_FOLDER_TYPE_IMPORTANT;

  return STAMP_FOLDER_TYPE_NORMAL;
}
