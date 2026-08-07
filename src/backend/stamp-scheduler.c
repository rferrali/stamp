/*
 * Copyright 2026 Jan-Michael Brummer
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

#include "config.h"

#include "stamp-scheduler.h"

#include <unistd.h>

#include <glib/gi18n.h>
#include <json-glib-1.0/json-glib/json-glib.h>

#include "stamp-session.h"

#define SCHEDULED_DIR "scheduled"
#define CHECK_INTERVAL_MS 30000

typedef struct {
  gchar *uuid;
  gint64 send_at;
  gchar *account_uid;
  gchar *sender_name;
  gchar *sender_mail;
  gchar *recipients_str;
  gboolean pgp_sign;
  gboolean pgp_encrypt;
  gboolean smime_sign;
  gboolean smime_encrypt;
  gchar *draft_uid;
} ScheduledEntry;

typedef struct {
  ScheduledEntry *entry;
  StampAccount *account;
} SendScheduledData;

static guint scheduler_timer_id = 0;
static gchar *scheduler_dir = NULL;

static gchar *
get_scheduler_dir (void)
{
  if (!scheduler_dir)
    scheduler_dir = g_build_path (G_DIR_SEPARATOR_S,
                                  g_get_user_data_dir (),
                                  "stamp",
                                  SCHEDULED_DIR,
                                  NULL);

  return scheduler_dir;
}

static gchar *
entry_get_json_path (const gchar *uuid)
{
  return g_strdup_printf ("%s%c%s.json",
                          get_scheduler_dir (),
                          G_DIR_SEPARATOR,
                          uuid);
}

static gchar *
entry_get_eml_path (const gchar *uuid)
{
  return g_strdup_printf ("%s%c%s.eml",
                          get_scheduler_dir (),
                          G_DIR_SEPARATOR,
                          uuid);
}

static void
scheduled_entry_free (gpointer ptr)
{
  ScheduledEntry *entry = ptr;

  g_free (entry->uuid);
  g_free (entry->account_uid);
  g_free (entry->sender_name);
  g_free (entry->sender_mail);
  g_free (entry->recipients_str);
  g_free (entry->draft_uid);
  g_free (entry);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ScheduledEntry, scheduled_entry_free)

static void
on_scheduled_send_mail (GObject      *account_obj,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  SendScheduledData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!stamp_account_send_mail_finish (STAMP_ACCOUNT (account_obj), res, &error)) {
    g_warning ("%s: Scheduled send failed: %s", G_STRFUNC, error->message);
    return;
  }

  if (data->entry->draft_uid)
    stamp_account_remove_draft (data->account, data->entry->draft_uid);
}

static GList *
load_scheduled_entries (void)
{
  GList *entries = NULL;
  const gchar *dir_path = get_scheduler_dir ();
  GDir *dir;

  dir = g_dir_open (dir_path, 0, NULL);
  if (!dir)
    return NULL;

  for (const gchar *name = g_dir_read_name (dir); name; name = g_dir_read_name (dir)) {
    g_autoptr (GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *json_data = NULL;
    g_autoptr (JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *obj;
    g_autofree char *uuid_str = NULL;
    const gchar *ext;
    ScheduledEntry *entry;

    ext = strrchr (name, '.');
    if (!ext || g_strcmp0 (ext, ".json") != 0)
      continue;

    uuid_str = g_strndup (name, ext - name);

    path = g_build_path (G_DIR_SEPARATOR_S, dir_path, name, NULL);
    if (!g_file_get_contents (path, &json_data, NULL, &error)) {
      g_warning ("%s: Failed to read %s: %s", G_STRFUNC, path, error->message);
      continue;
    }

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, json_data, -1, &error)) {
      g_warning ("%s: Failed to parse %s: %s", G_STRFUNC, path, error->message);
      continue;
    }

    root = json_parser_get_root (parser);
    if (!root || json_node_get_node_type (root) != JSON_NODE_OBJECT)
      continue;

    obj = json_node_get_object (root);
    entry = g_new0 (ScheduledEntry, 1);

    entry->uuid = g_steal_pointer (&uuid_str);
    entry->send_at = json_object_get_int_member_with_default (obj, "send_at", 0);
    entry->account_uid = g_strdup (json_object_get_string_member_with_default (obj, "account_uid", ""));
    entry->sender_name = g_strdup (json_object_get_string_member_with_default (obj, "sender_name", ""));
    entry->sender_mail = g_strdup (json_object_get_string_member_with_default (obj, "sender_mail", ""));
    entry->recipients_str = g_strdup (json_object_get_string_member_with_default (obj, "recipients", ""));
    entry->pgp_sign = json_object_get_boolean_member_with_default (obj, "pgp_sign", FALSE);
    entry->pgp_encrypt = json_object_get_boolean_member_with_default (obj, "pgp_encrypt", FALSE);
    entry->smime_sign = json_object_get_boolean_member_with_default (obj, "smime_sign", FALSE);
    entry->smime_encrypt = json_object_get_boolean_member_with_default (obj, "smime_encrypt", FALSE);
    entry->draft_uid = json_object_has_member (obj, "draft_uid")
                         ? g_strdup (json_object_get_string_member_with_default (obj, "draft_uid", NULL))
                         : NULL;

    entries = g_list_prepend (entries, entry);
  }

  g_dir_close (dir);

  return entries;
}

static gchar *
address_list_to_str (CamelInternetAddress *addr)
{
  g_autofree gchar *formatted = NULL;

  if (!addr || camel_address_length (CAMEL_ADDRESS (addr)) == 0)
    return g_strdup ("");

  formatted = camel_address_format (CAMEL_ADDRESS (addr));

  return g_steal_pointer (&formatted);
}

static CamelInternetAddress *
str_to_address_list (const gchar *str)
{
  CamelInternetAddress *addr;

  if (!str || !*str)
    return NULL;

  addr = camel_internet_address_new ();
  camel_address_decode (CAMEL_ADDRESS (addr), str);

  return addr;
}

gboolean
stamp_scheduler_schedule (StampAccount         *account,
                          CamelMimeMessage     *message,
                          const gchar          *sender_name,
                          const gchar          *sender_mail,
                          CamelInternetAddress *all_recipients,
                          gboolean              pgp_sign,
                          gboolean              pgp_encrypt,
                          gboolean              smime_sign,
                          gboolean              smime_encrypt,
                          const gchar          *draft_uid,
                          gint64                send_at)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *uuid = NULL;
  g_autofree char *json_path = NULL;
  g_autofree char *eml_path = NULL;
  g_autoptr (CamelStreamMem) mem_stream = NULL;
  g_autoptr (JsonBuilder) builder = NULL;
  g_autoptr (JsonGenerator) gen = NULL;
  g_autofree char *json_str = NULL;
  GByteArray *byte_array;
  g_autofree char *recipients_str = NULL;

  uuid = g_uuid_string_random ();

  if (g_mkdir_with_parents (get_scheduler_dir (), 0700) < 0) {
    g_warning ("%s: Failed to create scheduler directory", G_STRFUNC);
    return FALSE;
  }

  eml_path = entry_get_eml_path (uuid);
  mem_stream = CAMEL_STREAM_MEM (camel_stream_mem_new ());
  if (!camel_data_wrapper_write_to_stream_sync (CAMEL_DATA_WRAPPER (message),
                                                CAMEL_STREAM (mem_stream),
                                                NULL, &error)) {
    g_warning ("%s: Failed to serialize message: %s", G_STRFUNC, error->message);
    return FALSE;
  }

  byte_array = camel_stream_mem_get_byte_array (mem_stream);
  if (!g_file_set_contents (eml_path, (const gchar *)byte_array->data, byte_array->len, &error)) {
    g_warning ("%s: Failed to write message file: %s", G_STRFUNC, error->message);
    return FALSE;
  }

  recipients_str = address_list_to_str (all_recipients);

  builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "send_at");
  json_builder_add_int_value (builder, send_at);
  json_builder_set_member_name (builder, "account_uid");
  json_builder_add_string_value (builder, stamp_account_get_uid (account));
  json_builder_set_member_name (builder, "sender_name");
  json_builder_add_string_value (builder, sender_name ? sender_name : "");
  json_builder_set_member_name (builder, "sender_mail");
  json_builder_add_string_value (builder, sender_mail ? sender_mail : "");
  json_builder_set_member_name (builder, "recipients");
  json_builder_add_string_value (builder, recipients_str);
  json_builder_set_member_name (builder, "pgp_sign");
  json_builder_add_boolean_value (builder, pgp_sign);
  json_builder_set_member_name (builder, "pgp_encrypt");
  json_builder_add_boolean_value (builder, pgp_encrypt);
  json_builder_set_member_name (builder, "smime_sign");
  json_builder_add_boolean_value (builder, smime_sign);
  json_builder_set_member_name (builder, "smime_encrypt");
  json_builder_add_boolean_value (builder, smime_encrypt);
  if (draft_uid) {
    json_builder_set_member_name (builder, "draft_uid");
    json_builder_add_string_value (builder, draft_uid);
  }
  json_builder_end_object (builder);

  gen = json_generator_new ();
  json_generator_set_root (gen, json_builder_get_root (builder));
  json_str = json_generator_to_data (gen, NULL);

  json_path = entry_get_json_path (uuid);
  if (!g_file_set_contents (json_path, json_str, -1, &error)) {
    g_warning ("%s: Failed to write metadata file: %s", G_STRFUNC, error->message);
    unlink (eml_path);
    return FALSE;
  }

  return TRUE;
}

static void
send_scheduled_entry (ScheduledEntry *entry)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *eml_path = entry_get_eml_path (entry->uuid);
  g_autofree char *eml_data = NULL;
  gsize eml_len;
  g_autoptr (CamelStream) stream = NULL;
  g_autoptr (CamelMimeMessage) message = NULL;
  g_autoptr (CamelInternetAddress) sender = NULL;
  g_autoptr (CamelInternetAddress) recipients = NULL;
  StampAccount *account = NULL;
  SendScheduledData *send_data = NULL;
  GList *accounts;

  if (!g_file_get_contents (eml_path, &eml_data, &eml_len, &error)) {
    g_warning ("%s: Failed to read message file: %s", G_STRFUNC, error->message);
    return;
  }

  stream = camel_stream_mem_new_with_buffer (eml_data, eml_len);
  message = camel_mime_message_new ();
  if (!camel_data_wrapper_construct_from_stream_sync (CAMEL_DATA_WRAPPER (message),
                                                      stream, NULL, &error)) {
    g_warning ("%s: Failed to reconstruct message: %s", G_STRFUNC, error->message);
    return;
  }

  accounts = stamp_session_get_accounts (stamp_session_get_default ());
  for (GList *iter = accounts; iter && iter->data; iter = g_list_next (iter)) {
    StampAccount *cur = STAMP_ACCOUNT (iter->data);

    if (g_strcmp0 (stamp_account_get_uid (cur), entry->account_uid) == 0) {
      account = cur;
      break;
    }
  }

  if (!account) {
    g_warning ("%s: Account '%s' not found", G_STRFUNC, entry->account_uid);
    return;
  }

  sender = camel_internet_address_new ();
  camel_internet_address_add (sender, entry->sender_name, entry->sender_mail);

  recipients = str_to_address_list (entry->recipients_str);

  camel_medium_set_header (CAMEL_MEDIUM (message), "User-Agent", "Stamp (scheduled)");

  send_data = g_new0 (SendScheduledData, 1);
  send_data->entry = entry;
  send_data->account = g_object_ref (account);

  stamp_account_send_mail (account, message, sender, recipients,
                           entry->pgp_sign || entry->smime_sign,
                           entry->pgp_encrypt || entry->smime_encrypt,
                           NULL, on_scheduled_send_mail, send_data);
}

static void
cleanup_entry_files (ScheduledEntry *entry)
{
  g_autofree char *json_path = entry_get_json_path (entry->uuid);
  g_autofree char *eml_path = entry_get_eml_path (entry->uuid);

  unlink (json_path);
  unlink (eml_path);
}

static gboolean
check_scheduled_mails (gpointer user_data)
{
  g_autolist (ScheduledEntry) entries = load_scheduled_entries ();
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  for (GList *iter = entries; iter; ) {
    ScheduledEntry *entry = iter->data;
    GList *next = iter->next;

    if (entry->send_at <= now) {
      send_scheduled_entry (entry);
      cleanup_entry_files (entry);

      entries = g_list_delete_link (entries, iter);
    }

    iter = next;
  }

  return G_SOURCE_CONTINUE;
}

guint
stamp_scheduler_get_pending_count (void)
{
  g_autolist (ScheduledEntry) entries = load_scheduled_entries ();
  guint count = g_list_length (entries);

  return count;
}

gboolean
stamp_scheduler_init (void)
{
  if (scheduler_timer_id != 0)
    return TRUE;

  g_mkdir_with_parents (get_scheduler_dir (), 0700);

  scheduler_timer_id = g_timeout_add (CHECK_INTERVAL_MS, check_scheduled_mails, NULL);

  check_scheduled_mails (NULL);

  return TRUE;
}

void
stamp_scheduler_shutdown (void)
{
  g_clear_handle_id (&scheduler_timer_id, g_source_remove);

  g_clear_pointer (&scheduler_dir, g_free);
}
