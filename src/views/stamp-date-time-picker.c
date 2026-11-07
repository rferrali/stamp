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

#include "stamp-date-time-picker.h"

#include <gdesktop-enums.h>

struct _StampDateTimePicker {
  AdwPreferencesRow parent_instance;

  GtkMenuButton *date_button;
  GtkMenuButton *time_button;
  GtkCalendar *calendar;
  AdwSpinRow *hour;
  AdwSpinRow *minute;
  AdwComboRow *period;
  GSettings *clock_settings;
  gboolean use_12h;
  GDateTime *date_time;
  gchar *subtitle;
  gboolean show_date;
  gboolean show_time;
  gboolean updating;
};

G_DEFINE_FINAL_TYPE (StampDateTimePicker, stamp_date_time_picker, ADW_TYPE_PREFERENCES_ROW)

typedef enum {
  PROP_DATE_TIME = 1,
  PROP_SHOW_DATE,
  PROP_SHOW_TIME,
  PROP_SUBTITLE,
} StampDateTimePickerProps;

static GParamSpec *properties[PROP_SUBTITLE + 1];

static void
update_display (StampDateTimePicker *self)
{
  g_autofree gchar *date = g_date_time_format (self->date_time, "%x");
  g_autofree gchar *time = g_date_time_format (self->date_time, self->use_12h ? "%I:%M %p" : "%H:%M");
  g_autofree gchar *subtitle = NULL;

  self->updating = TRUE;

  gtk_menu_button_set_label (self->date_button, date);
  gtk_menu_button_set_label (self->time_button, time);

  gtk_widget_set_visible (GTK_WIDGET (self->date_button), self->show_date);
  gtk_widget_set_visible (GTK_WIDGET (self->time_button), self->show_time);

  gtk_calendar_set_date (self->calendar, self->date_time);
  adw_spin_row_set_range (self->hour, self->use_12h ? 1 : 0, self->use_12h ? 12 : 23);
  adw_spin_row_set_value (self->hour, self->use_12h ? (g_date_time_get_hour (self->date_time) + 11) % 12 + 1 : g_date_time_get_hour (self->date_time));
  gtk_widget_set_visible (GTK_WIDGET (self->period), self->use_12h);

  adw_combo_row_set_selected (self->period, g_date_time_get_hour (self->date_time) >= 12 ? 1 : 0);
  adw_spin_row_set_value (self->minute, g_date_time_get_minute (self->date_time));
  self->updating = FALSE;

  if (self->show_date && self->show_time)
    subtitle = g_strdup_printf ("%s %s", date, time);
  else
    subtitle = g_strdup (self->show_date ? date : self->show_time ? time : "");

  if (g_strcmp0 (self->subtitle, subtitle) != 0) {
    g_clear_pointer (&self->subtitle, g_free);
    self->subtitle = g_steal_pointer (&subtitle);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SUBTITLE]);
  }
}

GDateTime *
stamp_date_time_picker_get_date_time (StampDateTimePicker *self)
{
  g_return_val_if_fail (STAMP_IS_DATE_TIME_PICKER (self), NULL);
  return self->date_time;
}

void
stamp_date_time_picker_set_date_time (StampDateTimePicker *self,
                                      GDateTime           *date_time)
{
  g_autoptr (GDateTime) local = NULL;

  g_return_if_fail (STAMP_IS_DATE_TIME_PICKER (self));
  g_return_if_fail (date_time != NULL);

  local = g_date_time_to_local (date_time);
  if (self->date_time && g_date_time_equal (self->date_time, local))
    return;

  g_clear_pointer (&self->date_time, g_date_time_unref);
  self->date_time = g_steal_pointer (&local);
  update_display (self);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_DATE_TIME]);
}

static void
on_date_selected (GtkCalendar         *calendar,
                  StampDateTimePicker *self)
{
  g_autoptr (GDateTime) date = NULL;
  g_autoptr (GDateTime) selected = NULL;

  if (self->updating)
    return;

  date = gtk_calendar_get_date (calendar);
  selected = g_date_time_new_local (g_date_time_get_year (date),
                                    g_date_time_get_month (date),
                                    g_date_time_get_day_of_month (date),
                                    g_date_time_get_hour (self->date_time),
                                    g_date_time_get_minute (self->date_time), 0);

  if (selected)
    stamp_date_time_picker_set_date_time (self, selected);
}

static void
on_time_changed (GObject             *object,
                 GParamSpec          *pspec,
                 StampDateTimePicker *self)
{
  g_autoptr (GDateTime) selected = NULL;
  gint hour;

  if (self->updating)
    return;

  hour = (gint)adw_spin_row_get_value (self->hour);
  if (self->use_12h)
    hour = hour % 12 + (adw_combo_row_get_selected (self->period) == 1 ? 12 : 0);

  selected = g_date_time_new_local (g_date_time_get_year (self->date_time),
                                    g_date_time_get_month (self->date_time),
                                    g_date_time_get_day_of_month (self->date_time),
                                    hour,
                                    (gint)adw_spin_row_get_value (self->minute), 0);

  if (selected)
    stamp_date_time_picker_set_date_time (self, selected);
}

static void
on_time_closed (GtkPopover          *popover,
                StampDateTimePicker *self)
{
  self->updating = TRUE;

  adw_spin_row_update (self->hour);
  adw_spin_row_update (self->minute);
  self->updating = FALSE;

  on_time_changed (NULL, NULL, self);
}

static void
on_clock_format_changed (GSettings           *settings,
                         const gchar         *key,
                         StampDateTimePicker *self)
{
  self->use_12h = g_settings_get_enum (settings, "clock-format") == G_DESKTOP_CLOCK_FORMAT_12H;
  update_display (self);
}

static void
stamp_date_time_picker_get_property (GObject    *object,
                                     guint       property_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  StampDateTimePicker *self = STAMP_DATE_TIME_PICKER (object);

  switch ((StampDateTimePickerProps)property_id) {
    case PROP_DATE_TIME:
      g_value_set_boxed (value, self->date_time);
      break;
    case PROP_SHOW_DATE:
      g_value_set_boolean (value, self->show_date);
      break;
    case PROP_SHOW_TIME:
      g_value_set_boolean (value, self->show_time);
      break;
    case PROP_SUBTITLE:
      g_value_set_string (value, self->subtitle);
      break;
  }
}

static void
stamp_date_time_picker_set_property (GObject      *object,
                                     guint         property_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  StampDateTimePicker *self = STAMP_DATE_TIME_PICKER (object);

  switch ((StampDateTimePickerProps)property_id) {
    case PROP_DATE_TIME:
      stamp_date_time_picker_set_date_time (self, g_value_get_boxed (value));
      break;
    case PROP_SHOW_DATE:
      self->show_date = g_value_get_boolean (value);
      update_display (self);
      break;
    case PROP_SHOW_TIME:
      self->show_time = g_value_get_boolean (value);
      update_display (self);
      break;
    case PROP_SUBTITLE:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
stamp_date_time_picker_dispose (GObject *object)
{
  StampDateTimePicker *self = STAMP_DATE_TIME_PICKER (object);

  g_clear_object (&self->clock_settings);
  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_DATE_TIME_PICKER);
  G_OBJECT_CLASS (stamp_date_time_picker_parent_class)->dispose (object);
}

static void
stamp_date_time_picker_finalize (GObject *object)
{
  StampDateTimePicker *self = STAMP_DATE_TIME_PICKER (object);

  g_clear_pointer (&self->date_time, g_date_time_unref);
  g_clear_pointer (&self->subtitle, g_free);
  G_OBJECT_CLASS (stamp_date_time_picker_parent_class)->finalize (object);
}

static void
stamp_date_time_picker_class_init (StampDateTimePickerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = stamp_date_time_picker_get_property;
  object_class->set_property = stamp_date_time_picker_set_property;
  object_class->dispose = stamp_date_time_picker_dispose;
  object_class->finalize = stamp_date_time_picker_finalize;

  properties[PROP_DATE_TIME] = g_param_spec_boxed ("date-time", NULL, NULL, G_TYPE_DATE_TIME, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  properties[PROP_SHOW_DATE] = g_param_spec_boolean ("show-date", NULL, NULL, TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_SHOW_TIME] = g_param_spec_boolean ("show-time", NULL, NULL, TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_SUBTITLE] = g_param_spec_string ("subtitle", NULL, NULL, "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/stamp-date-time-picker.ui");
  gtk_widget_class_bind_template_child (widget_class, StampDateTimePicker, date_button);
  gtk_widget_class_bind_template_child (widget_class, StampDateTimePicker, time_button);
  gtk_widget_class_bind_template_child (widget_class, StampDateTimePicker, calendar);
  gtk_widget_class_bind_template_child (widget_class, StampDateTimePicker, hour);
  gtk_widget_class_bind_template_child (widget_class, StampDateTimePicker, minute);
  gtk_widget_class_bind_template_child (widget_class, StampDateTimePicker, period);
  gtk_widget_class_bind_template_callback (widget_class, on_date_selected);
  gtk_widget_class_bind_template_callback (widget_class, on_time_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_time_closed);
}

static void
stamp_date_time_picker_init (StampDateTimePicker *self)
{
  g_autoptr (GtkStringList) periods = gtk_string_list_new (NULL);
  g_autoptr (GDateTime) am = g_date_time_new_local (2000, 1, 1, 0, 0, 0);
  g_autoptr (GDateTime) pm = g_date_time_add_hours (am, 12);
  g_autofree gchar *am_label = g_date_time_format (am, "%p");
  g_autofree gchar *pm_label = g_date_time_format (pm, "%p");

  gtk_widget_init_template (GTK_WIDGET (self));

  self->updating = TRUE;
  self->show_date = TRUE;
  self->show_time = TRUE;
  self->date_time = g_date_time_new_now_local ();

  gtk_string_list_append (periods, am_label);
  gtk_string_list_append (periods, pm_label);
  adw_combo_row_set_model (self->period, G_LIST_MODEL (periods));

  self->clock_settings = g_settings_new ("org.gnome.desktop.interface");
  g_signal_connect_object (self->clock_settings, "changed::clock-format", G_CALLBACK (on_clock_format_changed), self, G_CONNECT_DEFAULT);
  on_clock_format_changed (self->clock_settings, "clock-format", self);
}

GtkWidget *
stamp_date_time_picker_new (void)
{
  return g_object_new (STAMP_TYPE_DATE_TIME_PICKER, NULL);
}
