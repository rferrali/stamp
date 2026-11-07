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
#include "stamp-out-of-office.h"

#include <evolution-ews/e-ews-connection.h>
#include <evolution-microsoft365/e-m365-connection.h>
#include <evolution-microsoft365/e-m365-tz-utils.h>
#include <glib/gi18n.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC (EEwsConnection, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (EEwsOofSettings, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (EM365Connection, g_object_unref)

static const gchar *
get_backend (StampAccount *account)
{
  StampMailService *mail = stamp_account_get_mail_service (account);
  ESource *source = mail ? stamp_mail_service_get_source (mail) : NULL;

  if (!source || !e_source_has_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT))
    return NULL;

  return e_source_backend_get_backend_name (e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT));
}

gboolean
stamp_out_of_office_supported (StampAccount *account)
{
  const gchar *backend = get_backend (account);

  if (g_strcmp0 (backend, "ews") == 0)
    return TRUE;

  if (g_strcmp0 (backend, "microsoft365") == 0)
    return TRUE;

  return FALSE;
}

void
stamp_out_of_office_free (StampOutOfOffice *settings)
{
  if (!settings)
    return;

  g_clear_pointer (&settings->start, g_date_time_unref);
  g_clear_pointer (&settings->end, g_date_time_unref);
  g_clear_pointer (&settings->internal_reply, g_free);
  g_clear_pointer (&settings->external_reply, g_free);
  g_free (settings);
}

static gboolean
exchange_settings (CamelService      *service,
                   ESource           *source,
                   StampOutOfOffice  *settings,
                   gboolean           save,
                   GCancellable      *cancellable,
                   GError           **error)
{
  g_autoptr (CamelSettings) camel_settings = camel_service_ref_settings (service);
  g_autofree gchar *host = camel_ews_settings_dup_hosturl (CAMEL_EWS_SETTINGS (camel_settings));
  g_autofree gchar *password = camel_service_dup_password (service);
  g_autoptr (GProxyResolver) proxy = camel_service_ref_proxy_resolver (service);
  g_autoptr (EEwsConnection) connection = NULL;
  g_autoptr (EEwsOofSettings) oof = NULL;
  gboolean success = TRUE;

  if (!host || !*host) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("Exchange server URL is missing"));
    return FALSE;
  }

  connection = e_ews_connection_new (source, host, CAMEL_EWS_SETTINGS (camel_settings));
  e_ews_connection_set_password (connection, password);
  e_ews_connection_set_proxy_resolver (connection, proxy);
  oof = e_ews_oof_settings_new_sync (connection, cancellable, error);

  if (!oof)
    return FALSE;

  if (save) {
    e_ews_oof_settings_set_state (oof, settings->state);
    e_ews_oof_settings_set_external_audience (oof, settings->audience);
    e_ews_oof_settings_set_internal_reply (oof, settings->internal_reply);
    e_ews_oof_settings_set_external_reply (oof, settings->external_reply);

    if (settings->state == STAMP_OUT_OF_OFFICE_STATE_SCHEDULED) {
      e_ews_oof_settings_set_start_time (oof, settings->start);
      e_ews_oof_settings_set_end_time (oof, settings->end);
    }

    success = e_ews_oof_settings_submit_sync (oof, cancellable, error);
  } else {
    settings->state = e_ews_oof_settings_get_state (oof);
    settings->audience = e_ews_oof_settings_get_external_audience (oof);
    settings->start = e_ews_oof_settings_ref_start_time (oof);
    settings->end = e_ews_oof_settings_ref_end_time (oof);
    settings->internal_reply = e_ews_oof_settings_dup_internal_reply (oof);
    settings->external_reply = e_ews_oof_settings_dup_external_reply (oof);
  }

  return success;
}

static GDateTime *
m365_date_time (EM365DateTimeWithZone *value)
{
  const gchar *date;
  const gchar *zone;
  g_autoptr (GTimeZone) mail_timezone = NULL;

  if (!value)
    return NULL;

  date = json_object_get_string_member_with_default (value, "dateTime", NULL);
  zone = e_m365_date_time_get_time_zone (value);
  mail_timezone = g_time_zone_new_identifier (zone && *zone ? zone : "UTC");
  if (!mail_timezone && zone) {
    const gchar *iana_zone;

    e_m365_tz_utils_ref_windows_zones ();

    iana_zone = e_m365_tz_utils_get_ical_equivalent (zone);
    if (iana_zone)
      mail_timezone = g_time_zone_new_identifier (iana_zone);

    e_m365_tz_utils_unref_windows_zones ();
  }

  if (!date || !mail_timezone)
    return NULL;

  return g_date_time_new_from_iso8601 (date, mail_timezone);
}

static gboolean
microsoft365_settings (CamelService      *service,
                       ESource           *source,
                       StampOutOfOffice  *settings,
                       gboolean           save,
                       GCancellable      *cancellable,
                       GError           **error)
{
  g_autoptr (CamelSettings) camel_settings = camel_service_ref_settings (service);
  g_autoptr (EM365Connection) connection = e_m365_connection_new (source, CAMEL_M365_SETTINGS (camel_settings));
  gboolean success;

  if (save) {
    g_autoptr (JsonBuilder) builder = json_builder_new ();

    e_m365_begin_mailbox_settings (builder);
    e_m365_begin_automatic_replies_setting (builder);
    e_m365_automatic_replies_setting_add_status (builder, E_M365_AUTOMATIC_REPLIES_STATUS_DISABLED + settings->state);
    e_m365_automatic_replies_setting_add_external_audience (builder, E_M365_EXTERNAL_AUDIENCE_SCOPE_NONE + settings->audience);
    e_m365_automatic_replies_setting_add_internal_reply_message (builder, settings->internal_reply);
    e_m365_automatic_replies_setting_add_external_reply_message (builder, settings->external_reply);

    if (settings->state == STAMP_OUT_OF_OFFICE_STATE_SCHEDULED) {
      e_m365_automatic_replies_setting_add_scheduled_start_date_time (builder, g_date_time_to_unix (settings->start), "UTC");
      e_m365_automatic_replies_setting_add_scheduled_end_date_time (builder, g_date_time_to_unix (settings->end), "UTC");
    }

    e_m365_end_automatic_replies_setting (builder);
    e_m365_end_mailbox_settings (builder);
    success = e_m365_connection_update_mailbox_settings_sync (connection, NULL, builder, cancellable, error);
  } else {
    g_autoptr (JsonObject) oof = NULL;

    success = e_m365_connection_get_automatic_replies_setting_sync (connection, NULL, &oof, cancellable, error);
    if (success && oof) {
      gint state = e_m365_automatic_replies_setting_get_status (oof) - E_M365_AUTOMATIC_REPLIES_STATUS_DISABLED;
      gint audience = e_m365_automatic_replies_setting_get_external_audience (oof) - E_M365_EXTERNAL_AUDIENCE_SCOPE_NONE;

      settings->state = CLAMP (state, STAMP_OUT_OF_OFFICE_STATE_DISABLED, STAMP_OUT_OF_OFFICE_STATE_SCHEDULED);
      settings->audience = CLAMP (audience, 0, 2);
      settings->start = m365_date_time (e_m365_automatic_replies_setting_get_scheduled_start_date_time (oof));
      settings->end = m365_date_time (e_m365_automatic_replies_setting_get_scheduled_end_date_time (oof));
      settings->internal_reply = g_strdup (e_m365_automatic_replies_setting_get_internal_reply_message (oof));
      settings->external_reply = g_strdup (e_m365_automatic_replies_setting_get_external_reply_message (oof));

      if (settings->state == STAMP_OUT_OF_OFFICE_STATE_SCHEDULED && (!settings->start || !settings->end)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The server returned an unsupported date or time zone"));
        success = FALSE;
      }
    } else if (success) {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The server returned no automatic reply settings"));
      success = FALSE;
    }
  }

  return success;
}

static void
settings_thread (GTask        *task,
                 gpointer      source_object,
                 gpointer      task_data,
                 GCancellable *cancellable)
{
  StampAccount *account = source_object;
  StampMailService *mail = stamp_account_get_mail_service (account);
  CamelService *service = mail ? stamp_mail_service_get_service (mail) : NULL;
  g_autoptr (StampOutOfOffice) loaded = NULL;
  StampOutOfOffice *settings = task_data;
  const gchar *backend = get_backend (account);
  gboolean save = settings != NULL;
  gboolean success = FALSE;
  g_autoptr (GError) error = NULL;

  if (!stamp_out_of_office_supported (account) || !service) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s", _("This account does not support automatic replies"));
    return;
  }

  if (save && (settings->state > STAMP_OUT_OF_OFFICE_STATE_SCHEDULED || settings->audience > 2 ||
               (settings->state == STAMP_OUT_OF_OFFICE_STATE_SCHEDULED && (!settings->start || !settings->end ||
                                                                           g_date_time_compare (settings->start, settings->end) >= 0)))) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s", _("Invalid automatic reply settings"));
    return;
  }

  if (!camel_service_connect_sync (service, cancellable, &error)) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  if (!save) {
    loaded = g_new0 (StampOutOfOffice, 1);
    settings = loaded;
  }

  if (g_strcmp0 (backend, "ews") == 0)
    success = exchange_settings (service, stamp_mail_service_get_source (mail), settings, save, cancellable, &error);
  else if (g_strcmp0 (backend, "microsoft365") == 0)
    success = microsoft365_settings (service, stamp_mail_service_get_source (mail), settings, save, cancellable, &error);

  if (!success) {
    if (error)
      g_task_return_error (task, g_steal_pointer (&error));
    else
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", _("Could not access automatic replies"));
  } else if (save) {
    g_task_return_boolean (task, TRUE);
  } else {
    g_task_return_pointer (task, g_steal_pointer (&loaded), (GDestroyNotify)stamp_out_of_office_free);
  }
}

void
stamp_out_of_office_load (StampAccount        *account,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (account, cancellable, callback, user_data);

  g_task_set_source_tag (task, stamp_out_of_office_load);
  g_task_run_in_thread (task, settings_thread);
}

StampOutOfOffice *
stamp_out_of_office_load_finish (StampAccount  *account,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, account), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

void
stamp_out_of_office_save (StampAccount        *account,
                          StampOutOfOffice    *settings,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (account, cancellable, callback, user_data);

  g_task_set_source_tag (task, stamp_out_of_office_save);
  g_task_set_task_data (task, settings, (GDestroyNotify)stamp_out_of_office_free);
  g_task_run_in_thread (task, settings_thread);
}

gboolean
stamp_out_of_office_save_finish (StampAccount  *account,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, account), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}
