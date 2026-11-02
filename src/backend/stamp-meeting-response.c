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

#include "stamp-meeting-response.h"

#include <glib/gi18n.h>

#include "stamp-calendar-autocleanups.h"
#include "stamp-m365-calendar.h"

ICalComponent *
stamp_meeting_response_ref_event (ICalComponent *calendar)
{
  return i_cal_component_isa (calendar) == I_CAL_VEVENT_COMPONENT ? g_object_ref (calendar) : i_cal_component_get_first_component (calendar, I_CAL_VEVENT_COMPONENT);
}

const gchar *
stamp_meeting_response_get_email (const gchar *value)
{
  return value && g_ascii_strncasecmp (value, "mailto:", 7) == 0 ? value + 7 : value;
}

ICalComponent *
stamp_meeting_response_new_reply (ICalComponent          *invitation,
                                  const gchar            *email,
                                  ICalParameterPartstat   response,
                                  GError                **error)
{
  g_autoptr (ICalComponent) reply = NULL;
  g_autoptr (ICalComponent) event = NULL;
  g_autoptr (ICalProperty) attendee = NULL;
  gboolean found = FALSE;

  if (!email || !*email || (response != I_CAL_PARTSTAT_ACCEPTED && response != I_CAL_PARTSTAT_DECLINED && response != I_CAL_PARTSTAT_TENTATIVE)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Invalid meeting response"));
    return NULL;
  }

  if (i_cal_component_isa (invitation) == I_CAL_VEVENT_COMPONENT) {
    reply = i_cal_component_new (I_CAL_VCALENDAR_COMPONENT);
    event = i_cal_component_clone (invitation);
    i_cal_component_add_component (reply, event);
  } else {
    reply = i_cal_component_clone (invitation);
    event = i_cal_component_get_first_component (reply, I_CAL_VEVENT_COMPONENT);
  }

  if (!event || !i_cal_component_get_uid (event) || !i_cal_component_count_properties (event, I_CAL_ORGANIZER_PROPERTY)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The invitation has no event ID or organizer"));
    return NULL;
  }

  for (attendee = i_cal_component_get_first_property (event, I_CAL_ATTENDEE_PROPERTY); attendee; attendee = i_cal_component_get_next_property (event, I_CAL_ATTENDEE_PROPERTY)) {
    const gchar *address = stamp_meeting_response_get_email (i_cal_property_get_attendee (attendee));

    if (address && g_ascii_strcasecmp (address, email) == 0) {
      g_autoptr (ICalParameter) partstat = i_cal_parameter_new_partstat (response);

      i_cal_property_set_parameter (attendee, partstat);
      found = TRUE;
    }

    g_clear_object (&attendee);
  }

  if (!found) {
    g_autofree gchar *address = g_strdup_printf ("mailto:%s", email);
    g_autoptr (ICalParameter) partstat = i_cal_parameter_new_partstat (response);
    g_autoptr (ICalParameter) rsvp = i_cal_parameter_new_rsvp (I_CAL_RSVP_FALSE);

    attendee = i_cal_property_new_attendee (address);
    i_cal_property_set_parameter (attendee, partstat);
    i_cal_property_set_parameter (attendee, rsvp);
    i_cal_component_add_property (event, attendee);
  }

  if (!i_cal_component_count_properties (reply, I_CAL_VERSION_PROPERTY)) {
    g_autoptr (ICalProperty) version = i_cal_property_new_version ("2.0");

    i_cal_component_add_property (reply, version);
  }

  i_cal_component_set_method (reply, I_CAL_METHOD_REPLY);

  return g_steal_pointer (&reply);
}

gboolean
stamp_meeting_response_wants_reply (ICalComponent *invitation,
                                    const gchar   *email)
{
  g_autoptr (ICalComponent) event = stamp_meeting_response_ref_event (invitation);
  g_autoptr (ICalProperty) attendee = NULL;

  if (!event || !email || !*email)
    return FALSE;

  for (attendee = i_cal_component_get_first_property (event, I_CAL_ATTENDEE_PROPERTY); attendee; attendee = i_cal_component_get_next_property (event, I_CAL_ATTENDEE_PROPERTY)) {
    const gchar *address = stamp_meeting_response_get_email (i_cal_property_get_attendee (attendee));

    if (address && g_ascii_strcasecmp (address, email) == 0) {
      g_autoptr (ICalParameter) rsvp = i_cal_property_get_first_parameter (attendee, I_CAL_RSVP_PARAMETER);

      if (rsvp && i_cal_parameter_get_rsvp (rsvp) == I_CAL_RSVP_TRUE)
        return TRUE;
    }

    g_clear_object (&attendee);
  }

  return FALSE;
}

gboolean
stamp_meeting_response_receive_sync (ECalClient     *client,
                                     ICalComponent  *reply,
                                     const gchar    *email,
                                     GCancellable   *cancellable,
                                     GError        **error)
{
  ECalOperationFlags flags = stamp_meeting_response_wants_reply (reply, email) ? E_CAL_OPERATION_FLAG_NONE : E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE;
  g_autoptr (GError) local_error = NULL;

  if (e_cal_client_receive_objects_sync (client, reply, flags, cancellable, &local_error))
    return TRUE;

  if (flags == E_CAL_OPERATION_FLAG_NONE && local_error) {
    g_autofree gchar *message = g_ascii_strdown (local_error->message, -1);

    if (strstr (message, "errorinvalidrequest") && (strstr (message, "organizer hasn't requested a response") || strstr (message, "organizer has not requested a response"))) {
      g_clear_error (&local_error);

      if (e_cal_client_receive_objects_sync (client, reply, E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE, cancellable, &local_error))
        return TRUE;
    }
  }

  if (!local_error)
    g_set_error_literal (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED, _("The meeting response could not be saved or sent"));

  g_propagate_error (error, g_steal_pointer (&local_error));

  return FALSE;
}

ESource *
stamp_meeting_response_choose_source (GList        *candidates,
                                      const gchar  *existing_uid,
                                      const gchar  *default_uid,
                                      GError      **error)
{
  const gchar *wanted = existing_uid ? existing_uid : default_uid;

  for (GList *iter = candidates; iter; iter = iter->next) {
    ESource *source = iter->data;

    if (e_source_get_enabled (source) && e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR) && wanted && g_strcmp0 (e_source_get_uid (source), wanted) == 0)
      return g_object_ref (source);
  }

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No suitable calendar is available for this account"));
  return NULL;
}

static ECalClient *
find_meeting_client_sync (ESourceRegistry  *registry,
                          const gchar      *account_uid,
                          ESource          *auth_source,
                          ICalComponent    *invitation,
                          gboolean          existing_only,
                          ICalComponent   **out_stored,
                          GCancellable     *cancellable,
                          GError          **error)
{
  g_autolist (ESource) all_sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_CALENDAR);
  g_autoptr (ESource) preferred = e_source_registry_ref_default_calendar (registry);
  g_autoptr (ESource) selected = NULL;
  g_autoptr (ICalComponent) event = NULL;
  g_autoptr (ICalTime) rid = NULL;
  g_autofree gchar *rid_string = NULL;
  g_autofree gchar *default_uid = NULL;
  g_autoptr (GError) lookup_error = NULL;
  g_autoptr (GError) default_error = NULL;
  g_autoptr (GList) candidates = NULL;
  const gchar *uid;

  event = stamp_meeting_response_ref_event (invitation);
  uid = event ? i_cal_component_get_uid (event) : NULL;
  if (!uid || !*uid) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The invitation has no event ID"));
    return NULL;
  }

  rid = i_cal_component_get_recurrenceid (event);
  if (rid && !i_cal_time_is_null_time (rid))
    rid_string = i_cal_time_as_ical_string (rid);

  if (out_stored)
    *out_stored = NULL;

  for (GList *iter = all_sources; iter; iter = iter->next) {
    ESource *source = iter->data;
    g_autoptr (ESource) collection = e_source_registry_find_extension (registry, source, E_SOURCE_EXTENSION_COLLECTION);

    if (g_strcmp0 (e_source_get_parent (source), account_uid) != 0 && (!collection || g_strcmp0 (e_source_get_uid (collection), account_uid) != 0))
      continue;

    candidates = g_list_append (candidates, source);

    if (preferred && g_strcmp0 (e_source_get_uid (source), e_source_get_uid (preferred)) == 0)
      default_uid = g_strdup (e_source_get_uid (source));
  }

  if (!default_uid && candidates && !candidates->next)
    default_uid = g_strdup (e_source_get_uid (candidates->data));

  if (!default_uid)
    default_uid = stamp_m365_calendar_get_default_uid_sync (candidates, auth_source, cancellable, &default_error);

  if (default_uid) {
    for (GList *iter = candidates; iter; iter = iter->next) {
      if (g_strcmp0 (e_source_get_uid (iter->data), default_uid) == 0) {
        gpointer source = iter->data;

        candidates = g_list_delete_link (candidates, iter);
        candidates = g_list_prepend (candidates, source);
        break;
      }
    }
  }

  for (GList *iter = candidates; iter; iter = iter->next) {
    ESource *source = iter->data;
    g_autoptr (EClient) client = NULL;
    g_autoptr (ICalComponent) stored = NULL;
    g_autoptr (GError) local_error = NULL;

    if (g_cancellable_set_error_if_cancelled (cancellable, error))
      return NULL;

    client = e_cal_client_connect_sync (source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 10, cancellable, &local_error);
    if (client) {
      e_cal_client_get_object_sync (E_CAL_CLIENT (client), uid, rid_string, &stored, cancellable, &local_error);

      if (!stored && existing_only && rid_string && g_error_matches (local_error, E_CAL_CLIENT_ERROR, E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND)) {
        g_clear_error (&local_error);
        e_cal_client_get_object_sync (E_CAL_CLIENT (client), uid, NULL, &stored, cancellable, &local_error);
      }
    }

    if (stored && existing_only && !stamp_meeting_response_contains_occurrence (invitation, stored))
      g_clear_object (&stored);

    if (client && stored) {
      if (!e_client_is_readonly (client)) {
        if (out_stored)
          *out_stored = g_steal_pointer (&stored);
        g_debug ("Meeting response: existing event in '%s'", e_source_get_display_name (source));
        return E_CAL_CLIENT (g_steal_pointer (&client));
      }

      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, _("The meeting calendar is read-only"));
      return NULL;
    }

    if (local_error && !g_error_matches (local_error, E_CAL_CLIENT_ERROR, E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND)) {
      g_clear_error (&lookup_error);
      lookup_error = g_steal_pointer (&local_error);
    }
  }

  if (lookup_error) {
    g_propagate_error (error, g_steal_pointer (&lookup_error));
    return NULL;
  }

  if (existing_only) {
    g_set_error_literal (error, E_CAL_CLIENT_ERROR, E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND, _("The meeting is no longer in the account calendars"));
    return NULL;
  }

  if (!default_uid && default_error) {
    g_propagate_error (error, g_steal_pointer (&default_error));
    return NULL;
  }

  selected = stamp_meeting_response_choose_source (candidates, NULL, default_uid, error);
  if (selected) {
    EClient *client = e_cal_client_connect_sync (selected, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 10, cancellable, error);

    if (client && e_client_is_readonly (client)) {
      g_object_unref (client);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, _("The default calendar is read-only"));
      return NULL;
    }

    return E_CAL_CLIENT (client);
  }

  return NULL;
}

ECalClient *
stamp_meeting_response_ref_client_sync (ESourceRegistry  *registry,
                                        const gchar      *account_uid,
                                        ESource          *auth_source,
                                        ICalComponent    *invitation,
                                        GCancellable     *cancellable,
                                        GError          **error)
{
  return find_meeting_client_sync (registry, account_uid, auth_source, invitation, FALSE, NULL, cancellable, error);
}

ECalClient *
stamp_meeting_response_ref_existing_client_sync (ESourceRegistry  *registry,
                                                 const gchar      *account_uid,
                                                 ESource          *auth_source,
                                                 ICalComponent    *invitation,
                                                 ICalComponent   **out_stored,
                                                 GCancellable     *cancellable,
                                                 GError          **error)
{
  return find_meeting_client_sync (registry, account_uid, auth_source, invitation, TRUE, out_stored, cancellable, error);
}

ECalComponentId *
stamp_meeting_response_get_removal_id (ICalComponent  *invitation,
                                       ICalComponent  *stored,
                                       GError        **error)
{
  g_autoptr (ICalComponent) event = stamp_meeting_response_ref_event (invitation);
  g_autoptr (ICalComponent) saved = stamp_meeting_response_ref_event (stored);
  g_autofree gchar *rid = NULL;
  const gchar *uid;

  uid = saved ? i_cal_component_get_uid (saved) : NULL;
  if (!event || !uid || !*uid) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The invitation has no event ID"));
    return NULL;
  }

  rid = e_cal_util_component_get_recurid_as_string (event);
  if (i_cal_component_count_properties (event, I_CAL_SEQUENCE_PROPERTY) && i_cal_component_count_properties (saved, I_CAL_SEQUENCE_PROPERTY) && i_cal_component_get_sequence (event) < i_cal_component_get_sequence (saved)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The calendar contains a newer version of this meeting"));
    return NULL;
  }

  if (!stamp_meeting_response_contains_occurrence (invitation, stored)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The cancellation date does not match the calendar entry"));
    return NULL;
  }

  return e_cal_component_id_new (uid, rid && *rid ? rid : NULL);
}

static ICalTimezone *
resolve_meeting_timezone (const gchar   *tzid,
                          gpointer       user_data,
                          GCancellable  *cancellable,
                          GError       **error)
{
  ICalTimezone *zone = i_cal_component_get_timezone (user_data, tzid);

  if (!zone)
    zone = i_cal_timezone_get_builtin_timezone_from_tzid (tzid);
  if (!zone)
    zone = i_cal_timezone_get_builtin_timezone (tzid);
  return zone;
}

static ICalTime *
meeting_time (ICalComponent     *calendar,
              ICalComponent     *event,
              ICalPropertyKind   kind,
              gchar            **out_tzid)
{
  g_autoptr (ICalProperty) property = i_cal_component_get_first_property (event, kind);
  g_autoptr (ICalParameter) parameter = NULL;
  ICalTime *time;

  *out_tzid = NULL;
  if (!property)
    return NULL;

  time = i_cal_property_get_datetime_with_component (property, calendar);
  if (!time || i_cal_time_is_null_time (time) || !i_cal_time_is_valid_time (time)) {
    g_clear_object (&time);
    return NULL;
  }

  parameter = i_cal_property_get_first_parameter (property, I_CAL_TZID_PARAMETER);
  if (parameter)
    *out_tzid = g_strdup (i_cal_parameter_get_tzid (parameter));

  if (!i_cal_time_is_date (time) && !i_cal_time_get_timezone (time) && *out_tzid) {
    ICalTimezone *zone = resolve_meeting_timezone (*out_tzid, calendar, NULL, NULL);

    if (zone)
      i_cal_time_set_timezone (time, zone);
  }

  return time;
}

static gboolean
meeting_times_equal (ICalTime    *first,
                     const gchar *first_tzid,
                     ICalTime    *second,
                     const gchar *second_tzid)
{
  ICalTimezone *first_zone;
  ICalTimezone *second_zone;

  if (!first || !second || i_cal_time_is_date (first) != i_cal_time_is_date (second))
    return FALSE;

  if (i_cal_time_is_date (first))
    return i_cal_time_compare_date_only (first, second) == 0;

  first_zone = i_cal_time_get_timezone (first);
  second_zone = i_cal_time_get_timezone (second);
  if (first_zone && second_zone)
    return i_cal_time_as_timet_with_zone (first, first_zone) == i_cal_time_as_timet_with_zone (second, second_zone);

  return !first_zone && !second_zone && g_strcmp0 (first_tzid, second_tzid) == 0 && i_cal_time_compare (first, second) == 0;
}

typedef struct {
  ICalTime *rid;
  ICalTime *start;
  ICalTime *end;
  const gchar *rid_tzid;
  const gchar *start_tzid;
  const gchar *end_tzid;
  gboolean found;
} OccurrenceMatch;

static gboolean
match_generated_occurrence (ICalComponent  *event,
                            ICalTime       *instance_start,
                            ICalTime       *instance_end,
                            gpointer        user_data,
                            GCancellable   *cancellable,
                            GError        **error)
{
  OccurrenceMatch *match = user_data;

  match->found = meeting_times_equal (match->rid, match->rid_tzid, instance_start, NULL) && (!match->start || meeting_times_equal (match->start, match->start_tzid, instance_start, NULL)) && (!match->end || meeting_times_equal (match->end, match->end_tzid, instance_end, NULL));
  return !match->found;
}

gboolean
stamp_meeting_response_contains_occurrence (ICalComponent *invitation,
                                            ICalComponent *stored)
{
  g_autoptr (ICalComponent) event = stamp_meeting_response_ref_event (invitation);
  g_autoptr (ICalComponent) saved = stamp_meeting_response_ref_event (stored);
  g_autoptr (ICalTime) rid = NULL;
  g_autoptr (ICalTime) saved_rid = NULL;
  g_autoptr (ICalTime) start = NULL;
  g_autoptr (ICalTime) saved_start = NULL;
  g_autoptr (ICalTime) end = NULL;
  g_autoptr (ICalTime) saved_end = NULL;
  g_autofree gchar *rid_tzid = NULL;
  g_autofree gchar *saved_rid_tzid = NULL;
  g_autofree gchar *start_tzid = NULL;
  g_autofree gchar *saved_start_tzid = NULL;
  g_autofree gchar *end_tzid = NULL;
  g_autofree gchar *saved_end_tzid = NULL;
  g_autoptr (ICalTime) interval_end = NULL;
  ICalTimezone *default_zone;
  OccurrenceMatch match;

  if (!event || !saved)
    return FALSE;

  rid = meeting_time (invitation, event, I_CAL_RECURRENCEID_PROPERTY, &rid_tzid);
  saved_rid = meeting_time (stored, saved, I_CAL_RECURRENCEID_PROPERTY, &saved_rid_tzid);
  start = meeting_time (invitation, event, I_CAL_DTSTART_PROPERTY, &start_tzid);
  saved_start = meeting_time (stored, saved, I_CAL_DTSTART_PROPERTY, &saved_start_tzid);
  end = meeting_time (invitation, event, I_CAL_DTEND_PROPERTY, &end_tzid);
  saved_end = meeting_time (stored, saved, I_CAL_DTEND_PROPERTY, &saved_end_tzid);

  if ((i_cal_component_count_properties (event, I_CAL_DTSTART_PROPERTY) && !start) || (i_cal_component_count_properties (event, I_CAL_RECURRENCEID_PROPERTY) && !rid) || (i_cal_component_count_properties (saved, I_CAL_RECURRENCEID_PROPERTY) && !saved_rid))
    return FALSE;

  if (!rid || saved_rid) {
    if (rid && (!saved_rid || !meeting_times_equal (rid, rid_tzid, saved_rid, saved_rid_tzid)))
      return FALSE;

    if (!rid && saved_rid)
      return FALSE;

    if (start && !meeting_times_equal (start, start_tzid, saved_start, saved_start_tzid))
      return FALSE;

    if (end && saved_end && !meeting_times_equal (end, end_tzid, saved_end, saved_end_tzid))
      return FALSE;

    return rid || start || (i_cal_component_get_uid (event) && g_strcmp0 (i_cal_component_get_uid (event), i_cal_component_get_uid (saved)) == 0);
  }

  if (!saved_start || (!i_cal_component_count_properties (saved, I_CAL_RRULE_PROPERTY) && !i_cal_component_count_properties (saved, I_CAL_RDATE_PROPERTY)))
    return FALSE;

  if (i_cal_time_is_date (rid) != i_cal_time_is_date (saved_start))
    return FALSE;

  interval_end = i_cal_time_clone (rid);
  default_zone = i_cal_time_get_timezone (saved_start);
  match = (OccurrenceMatch) {
    rid, start, end, rid_tzid, start_tzid, end_tzid, FALSE
  };

  if (!default_zone)
    default_zone = i_cal_timezone_get_utc_timezone ();

  if (!i_cal_time_is_date (rid) && !i_cal_time_get_timezone (rid) && !rid_tzid) {
    i_cal_time_set_timezone (rid, default_zone);
    i_cal_time_set_timezone (interval_end, default_zone);
  }

  if (start && !i_cal_time_is_date (start) && !i_cal_time_get_timezone (start) && !start_tzid)
    i_cal_time_set_timezone (start, default_zone);

  if (end && !i_cal_time_is_date (end) && !i_cal_time_get_timezone (end) && !end_tzid)
    i_cal_time_set_timezone (end, default_zone);

  i_cal_time_adjust (interval_end, 1, 0, 0, 0);
  e_cal_recur_generate_instances_sync (saved, rid, interval_end, match_generated_occurrence, &match, resolve_meeting_timezone, stored, default_zone, NULL, NULL);

  return match.found;
}
