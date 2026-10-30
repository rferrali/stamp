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
#include "stamp-m365-calendar.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

static gchar *
source_folder_id (ESource *source)
{
  g_autofree gchar *data = e_source_to_string (source, NULL);
  g_autoptr (GKeyFile) key_file = g_key_file_new ();

  if (!g_key_file_load_from_data (key_file, data, -1, G_KEY_FILE_NONE, NULL))
    return NULL;

  return g_key_file_get_string (key_file, "Microsoft365 Folder", "Id", NULL);
}

static gchar *
m365_default_folder_id (ESource       *source,
                        GCancellable  *cancellable,
                        GError       **error)
{
  g_autoptr (SoupSession) session = NULL;
  g_autoptr (SoupMessage) message = NULL;
  g_autoptr (GBytes) body = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autofree gchar *token = NULL;
  g_autofree gchar *authorization = NULL;
  gconstpointer data;
  gsize length;
  JsonNode *root;
  const gchar *id;

  if (!e_source_get_oauth2_access_token_sync (source, cancellable, &token, NULL, error))
    return NULL;

  session = soup_session_new_with_options ("timeout", 30, NULL);
  message = soup_message_new ("GET", "https://graph.microsoft.com/v1.0/me/calendar?$select=id");
  authorization = g_strdup_printf ("Bearer %s", token);
  soup_message_headers_replace (soup_message_get_request_headers (message), "Authorization", authorization);

  body = soup_session_send_and_read (session, message, cancellable, error);
  if (!body)
    return NULL;

  if (!SOUP_STATUS_IS_SUCCESSFUL (soup_message_get_status (message))) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Could not determine the default calendar (HTTP %u)"), soup_message_get_status (message));
    return NULL;
  }

  data = g_bytes_get_data (body, &length);
  if (!json_parser_load_from_data (parser, data, length, error))
    return NULL;

  root = json_parser_get_root (parser);
  id = JSON_NODE_HOLDS_OBJECT (root) ? json_object_get_string_member_with_default (json_node_get_object (root), "id", NULL) : NULL;
  if (!id || !*id) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The server returned no default calendar"));
    return NULL;
  }

  return g_strdup (id);
}

gchar *
stamp_m365_calendar_get_default_uid_sync (GList         *candidates,
                                          ESource       *auth_source,
                                          GCancellable  *cancellable,
                                          GError       **error)
{
  g_autofree gchar *folder_id = NULL;
  gboolean is_m365 = FALSE;

  for (GList *iter = candidates; iter; iter = iter->next) {
    ESourceBackend *backend = e_source_get_extension (iter->data, E_SOURCE_EXTENSION_CALENDAR);

    is_m365 |= g_strcmp0 (e_source_backend_get_backend_name (backend), "microsoft365") == 0;
  }

  if (!is_m365)
    return NULL;

  folder_id = m365_default_folder_id (auth_source, cancellable, error);
  for (GList *iter = candidates; folder_id && iter; iter = iter->next) {
    g_autofree gchar *id = source_folder_id (iter->data);

    if (g_strcmp0 (id, folder_id) == 0)
      return g_strdup (e_source_get_uid (iter->data));
  }

  return NULL;
}
