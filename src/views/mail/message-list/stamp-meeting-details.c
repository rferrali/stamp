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

#include "stamp-meeting-details.h"

#include <glib/gi18n.h>

#include "stamp-calendar-autocleanups.h"
#include "stamp-meeting-response.h"
#include "stamp-tag.h"

struct _StampMeetingDetails {
  GtkBox parent_instance;

  GtkLabel *heading;
  GtkLabel *summary;
  GtkLabel *when;
  GtkLabel *recurrence;
  GtkLabel *applies;
  GtkLabel *location;
  GtkLabel *response;
  GtkBox *organizer;
};

G_DEFINE_FINAL_TYPE (StampMeetingDetails, stamp_meeting_details, GTK_TYPE_BOX);

static void
stamp_meeting_details_dispose (GObject *object)
{
  gtk_widget_dispose_template (GTK_WIDGET (object), STAMP_TYPE_MEETING_DETAILS);
  G_OBJECT_CLASS (stamp_meeting_details_parent_class)->dispose (object);
}

static void
stamp_meeting_details_class_init (StampMeetingDetailsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = stamp_meeting_details_dispose;
  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/message-list/stamp-meeting-details.ui");

  gtk_widget_class_bind_template_child (widget_class, StampMeetingDetails, heading);
  gtk_widget_class_bind_template_child (widget_class, StampMeetingDetails, summary);
  gtk_widget_class_bind_template_child (widget_class, StampMeetingDetails, when);
  gtk_widget_class_bind_template_child (widget_class, StampMeetingDetails, recurrence);
  gtk_widget_class_bind_template_child (widget_class, StampMeetingDetails, applies);
  gtk_widget_class_bind_template_child (widget_class, StampMeetingDetails, location);
  gtk_widget_class_bind_template_child (widget_class, StampMeetingDetails, response);
  gtk_widget_class_bind_template_child (widget_class, StampMeetingDetails, organizer);
}

static void
stamp_meeting_details_init (StampMeetingDetails *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

static void
set_detail (GtkLabel    *label,
            const gchar *value)
{
  gtk_label_set_text (label, value ? value : "");
  gtk_widget_set_visible (gtk_widget_get_parent (GTK_WIDGET (label)), value && *value);
}

void
stamp_meeting_details_set_mobile_mode (GtkWidget *details,
                                       gboolean   mobile)
{
  for (GtkWidget *row = gtk_widget_get_first_child (details); row; row = gtk_widget_get_next_sibling (row)) {
    GtkWidget *title;

    if (!gtk_widget_has_css_class (row, "meeting-detail-row"))
      continue;

    title = gtk_widget_get_first_child (row);
    gtk_orientable_set_orientation (GTK_ORIENTABLE (row), mobile ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
    gtk_box_set_spacing (GTK_BOX (row), mobile ? 2 : 12);
    gtk_label_set_width_chars (GTK_LABEL (title), mobile ? 0 : 12);
  }
}

static const gchar *
participant_status (ICalParameterPartstat response)
{
  if (response == I_CAL_PARTSTAT_ACCEPTED)
    return _("Accepted");

  if (response == I_CAL_PARTSTAT_DECLINED)
    return _("Declined");

  if (response == I_CAL_PARTSTAT_TENTATIVE)
    return _("Tentative");

  if (response == I_CAL_PARTSTAT_DELEGATED)
    return _("Delegated");

  return _("No response yet");
}

static ICalParameterPartstat
get_response (ICalComponent *event,
              const gchar   *email)
{
  g_autoptr (ICalProperty) property = NULL;

  if (!email || !*email)
    return I_CAL_PARTSTAT_NONE;

  for (property = i_cal_component_get_first_property (event, I_CAL_ATTENDEE_PROPERTY); property; property = i_cal_component_get_next_property (event, I_CAL_ATTENDEE_PROPERTY)) {
    const gchar *address = stamp_meeting_response_get_email (i_cal_property_get_attendee (property));

    if (address && g_ascii_strcasecmp (email, address) == 0) {
      g_autoptr (ICalParameter) parameter = i_cal_property_get_first_parameter (property, I_CAL_PARTSTAT_PARAMETER);

      return parameter ? i_cal_parameter_get_partstat (parameter) : I_CAL_PARTSTAT_NEEDSACTION;
    }

    g_clear_object (&property);
  }

  return I_CAL_PARTSTAT_NONE;
}

ICalParameterPartstat
stamp_meeting_details_get_saved_response (ICalComponent *invitation,
                                          ICalComponent *stored,
                                          const gchar   *email,
                                          gboolean       uid_resolved)
{
  g_autoptr (ICalComponent) event = stamp_meeting_response_ref_event (invitation);
  g_autoptr (ICalComponent) saved = stamp_meeting_response_ref_event (stored);
  g_autoptr (ICalTime) rid = NULL;
  g_autoptr (ICalTime) saved_rid = NULL;
  const gchar *uid;
  ICalParameterPartstat response;

  if (!event || !saved)
    return I_CAL_PARTSTAT_NONE;

  uid = i_cal_component_get_uid (event);
  if (!uid || !*uid || (!uid_resolved && g_strcmp0 (uid, i_cal_component_get_uid (saved)) != 0))
    return I_CAL_PARTSTAT_NONE;

  rid = i_cal_component_get_recurrenceid (event);
  saved_rid = i_cal_component_get_recurrenceid (saved);
  if (rid && !i_cal_time_is_null_time (rid)) {
    if (!saved_rid || i_cal_time_is_null_time (saved_rid) || i_cal_time_compare (rid, saved_rid) != 0)
      return I_CAL_PARTSTAT_NONE;
  } else if (saved_rid && !i_cal_time_is_null_time (saved_rid)) {
    return I_CAL_PARTSTAT_NONE;
  }

  response = get_response (saved, email);
  if (response == I_CAL_PARTSTAT_NONE)
    return I_CAL_PARTSTAT_NONE;
  if (!stamp_meeting_response_contains_occurrence (invitation, stored))
    return I_CAL_PARTSTAT_NEEDSACTION;
  if (i_cal_component_count_properties (saved, I_CAL_SEQUENCE_PROPERTY)) {
    if (i_cal_component_get_sequence (saved) < i_cal_component_get_sequence (event))
      return I_CAL_PARTSTAT_NEEDSACTION;
  } else {
    g_autoptr (ICalTime) sent = i_cal_component_get_dtstamp (event);
    g_autoptr (ICalProperty) modified_property = i_cal_component_get_first_property (saved, I_CAL_LASTMODIFIED_PROPERTY);
    g_autoptr (ICalTime) modified = modified_property ? i_cal_property_get_lastmodified (modified_property) : NULL;

    if (sent && modified && !i_cal_time_is_null_time (sent) && !i_cal_time_is_null_time (modified) && i_cal_time_compare (modified, sent) < 0)
      return I_CAL_PARTSTAT_NEEDSACTION;
  }

  return response;
}

static GDateTime *
meeting_date (ICalComponent     *calendar,
              ICalComponent     *event,
              ICalPropertyKind   kind,
              gboolean           inclusive_end,
              gboolean          *all_day,
              gchar            **zone_label)
{
  g_autoptr (ICalProperty) property = i_cal_component_get_first_property (event, kind);
  g_autoptr (ICalTime) time = NULL;
  g_autoptr (ICalParameter) parameter = NULL;
  ICalTimezone *zone = NULL;
  g_autoptr (GDateTime) date = NULL;
  const gchar *tzid = NULL;

  *zone_label = NULL;
  *all_day = FALSE;
  if (!property)
    return NULL;

  time = i_cal_property_get_datetime_with_component (property, calendar);
  if (!time || i_cal_time_is_null_time (time) || !i_cal_time_is_valid_time (time))
    return NULL;

  *all_day = i_cal_time_is_date (time);
  if (inclusive_end && *all_day)
    i_cal_time_adjust (time, -1, 0, 0, 0);

  parameter = i_cal_property_get_first_parameter (property, I_CAL_TZID_PARAMETER);
  if (parameter)
    tzid = i_cal_parameter_get_tzid (parameter);

  zone = i_cal_time_get_timezone (time);
  if (!zone && tzid)
    zone = i_cal_timezone_get_builtin_timezone_from_tzid (tzid);

  if (!zone && tzid)
    zone = i_cal_timezone_get_builtin_timezone (tzid);

  if (!i_cal_time_is_date (time) && (zone || i_cal_time_is_utc (time))) {
    g_autoptr (GDateTime) utc = g_date_time_new_from_unix_utc (i_cal_time_as_timet_with_zone (time, zone));

    if (utc)
      date = g_date_time_to_local (utc);

    if (date)
      *zone_label = g_date_time_format (date, "%Z");

    return g_steal_pointer (&date);
  }

  date = g_date_time_new_utc (i_cal_time_get_year (time), i_cal_time_get_month (time), i_cal_time_get_day (time), i_cal_time_get_hour (time), i_cal_time_get_minute (time), 0);
  if (!date)
    return NULL;

  if (!*all_day)
    *zone_label = g_strdup (tzid ? tzid : _("local time"));

  return g_steal_pointer (&date);
}

static gchar *
format_period (ICalComponent *calendar,
               ICalComponent *event)
{
  g_autoptr (GDateTime) start = NULL;
  g_autoptr (GDateTime) end = NULL;
  g_autofree gchar *start_zone = NULL;
  g_autofree gchar *end_zone = NULL;
  g_autofree gchar *start_day = NULL;
  g_autofree gchar *end_day = NULL;
  g_autofree gchar *start_time = NULL;
  g_autofree gchar *end_time = NULL;
  gboolean start_all_day;
  gboolean end_all_day;
  gboolean same_day;

  start = meeting_date (calendar, event, I_CAL_DTSTART_PROPERTY, FALSE, &start_all_day, &start_zone);
  end = meeting_date (calendar, event, I_CAL_DTEND_PROPERTY, TRUE, &end_all_day, &end_zone);
  if (!start)
    return NULL;

  start_day = g_date_time_format (start, _("%a, %-d %b %Y"));
  same_day = end && g_date_time_get_year (start) == g_date_time_get_year (end) && g_date_time_get_day_of_year (start) == g_date_time_get_day_of_year (end);

  if (end)
    end_day = g_date_time_format (end, _("%a, %-d %b %Y"));

  if (start_all_day) {
    g_autofree gchar *days = end && end_all_day && !same_day ? g_strdup_printf ("%s – %s", start_day, end_day) : g_strdup (start_day);

    return g_strdup_printf (_("%s (all day)"), days);
  }

  start_time = g_date_time_format (start, "%H:%M");
  if (!end || end_all_day)
    return g_strdup_printf ("%s · %s (%s)", start_day, start_time, start_zone);

  end_time = g_date_time_format (end, "%H:%M");
  if (same_day && g_strcmp0 (start_zone, end_zone) == 0)
    return g_strdup_printf ("%s · %s–%s (%s)", start_day, start_time, end_time, end_zone);

  return g_strdup_printf ("%s · %s (%s) – %s · %s (%s)", start_day, start_time, start_zone, end_day, end_time, end_zone);
}

gboolean
stamp_meeting_details_is_cancelled (ICalComponent *calendar,
                                    const gchar   *method)
{
  g_autoptr (ICalComponent) event = stamp_meeting_response_ref_event (calendar);
  ICalPropertyMethod calendar_method = method ? i_cal_property_method_from_string (method) : i_cal_component_get_method (calendar);

  return event && (calendar_method == I_CAL_METHOD_CANCEL || i_cal_component_get_status (event) == I_CAL_STATUS_CANCELLED);
}

GtkWidget *
stamp_meeting_details_new (StampAccount          *account,
                           ICalComponent         *calendar,
                           const gchar           *method,
                           const gchar           *email,
                           ICalParameterPartstat  saved_response,
                           gboolean               response_pending,
                           gboolean              *can_respond)
{
  g_autoptr (ICalComponent) event = NULL;
  g_autoptr (ICalProperty) property = NULL;
  g_autofree gchar *period = NULL;
  StampMeetingDetails *self;
  const gchar *summary;
  const gchar *heading;
  ICalParameterPartstat response;
  ICalPropertyMethod calendar_method;
  gboolean cancelled;

  *can_respond = FALSE;
  event = stamp_meeting_response_ref_event (calendar);
  if (!event)
    return NULL;

  calendar_method = method ? i_cal_property_method_from_string (method) : i_cal_component_get_method (calendar);
  cancelled = stamp_meeting_details_is_cancelled (calendar, method);
  response = saved_response != I_CAL_PARTSTAT_NONE ? saved_response : get_response (event, email);
  *can_respond = !response_pending && calendar_method == I_CAL_METHOD_REQUEST && !cancelled && response != I_CAL_PARTSTAT_ACCEPTED && response != I_CAL_PARTSTAT_DECLINED && response != I_CAL_PARTSTAT_TENTATIVE && response != I_CAL_PARTSTAT_DELEGATED;
  heading = cancelled ? _("Meeting cancelled") : calendar_method == I_CAL_METHOD_REPLY ? _("Meeting response") : calendar_method == I_CAL_METHOD_REQUEST ? _("Meeting invitation") : _("Calendar event");

  self = g_object_new (STAMP_TYPE_MEETING_DETAILS, NULL);
  gtk_label_set_text (self->heading, heading);
  gtk_widget_add_css_class (GTK_WIDGET (self->heading), cancelled ? "error" : "accent");
  summary = i_cal_component_get_summary (event);
  gtk_label_set_text (self->summary, summary && *summary ? summary : _("Untitled meeting"));

  period = format_period (calendar, event);
  if (period && (i_cal_component_count_properties (event, I_CAL_DTEND_PROPERTY) || i_cal_component_count_properties (event, I_CAL_DURATION_PROPERTY))) {
    g_autoptr (ICalDuration) duration = i_cal_component_get_duration (event);
    gint seconds = duration ? i_cal_duration_as_int (duration) : 0;

    if (seconds > 0) {
      g_autoptr (ICalTime) dtstart = i_cal_component_get_dtstart (event);
      g_autofree gchar *length = NULL;
      gchar *combined;

      if (dtstart && i_cal_time_is_date (dtstart)) {
        guint days = seconds / 86400;

        length = g_strdup_printf (ngettext ("%u day", "%u days", days), days);
      } else {
        guint minutes = seconds / 60;

        if (minutes >= 60) {
          guint hours = minutes / 60;
          g_autofree gchar *hours_text = g_strdup_printf (ngettext ("%u hour", "%u hours", hours), hours);

          minutes %= 60;
          if (minutes) {
            g_autofree gchar *minutes_text = g_strdup_printf (ngettext ("%u minute", "%u minutes", minutes), minutes);

            /* Translators: hours followed by the remaining minutes. */
            length = g_strdup_printf (_("%s %s"), hours_text, minutes_text);
          } else {
            length = g_steal_pointer (&hours_text);
          }
        } else {
          length = g_strdup_printf (ngettext ("%u minute", "%u minutes", minutes), minutes);
        }
      }

      combined = g_strdup_printf ("%s · %s", period, length);
      g_free (period);
      period = combined;
    }
  }

  set_detail (self->when, period ? period : _("Date and time not specified"));

  if (i_cal_component_count_properties (event, I_CAL_RRULE_PROPERTY) || i_cal_component_count_properties (event, I_CAL_RDATE_PROPERTY)) {
    g_autofree gchar *recurrence = e_cal_recur_describe_recurrence (event, G_DATE_MONDAY, E_CAL_RECUR_DESCRIBE_RECURRENCE_FLAG_FALLBACK);

    set_detail (self->recurrence, recurrence);
  }

  if (i_cal_component_count_properties (event, I_CAL_RECURRENCEID_PROPERTY))
    set_detail (self->applies, _("This occurrence only"));

  set_detail (self->location, i_cal_component_get_location (event));
  property = i_cal_component_get_first_property (event, I_CAL_ORGANIZER_PROPERTY);
  if (property) {
    g_autoptr (ICalParameter) cn = i_cal_property_get_first_parameter (property, I_CAL_CN_PARAMETER);
    const gchar *name = cn ? i_cal_parameter_get_cn (cn) : NULL;
    const gchar *address = stamp_meeting_response_get_email (i_cal_property_get_organizer (property));

    if ((name && *name) || (address && *address)) {
      GtkWidget *tag = stamp_tag_new (account);

      stamp_tag_set_label (STAMP_TAG (tag), name && *name ? name : address);
      stamp_tag_set_mail (STAMP_TAG (tag), address);
      stamp_tag_set_show_button (STAMP_TAG (tag), FALSE);
      gtk_box_append (self->organizer, tag);
      gtk_widget_set_visible (gtk_widget_get_parent (GTK_WIDGET (self->organizer)), TRUE);
    }
  }

  g_clear_object (&property);

  if (!cancelled && !response_pending && response != I_CAL_PARTSTAT_NONE)
    set_detail (self->response, participant_status (response));

  return GTK_WIDGET (self);
}
