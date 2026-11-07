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

#include "stamp-preferences-out-of-office.h"

#include <glib/gi18n.h>

#include "stamp-date-time-picker.h"
#include "stamp-out-of-office.h"
#include "stamp-rich-text-editor.h"

struct _StampPreferencesOutOfOffice {
  AdwNavigationPage parent_instance;

  AdwComboRow *oof_state;
  AdwComboRow *oof_audience;
  StampDateTimePicker *oof_start;
  StampDateTimePicker *oof_end;
  StampRichTextEditor *oof_internal;
  StampRichTextEditor *oof_external;
  AdwActionRow *oof_internal_link;
  AdwActionRow *oof_external_link;
  guint internal_preview_generation;
  guint external_preview_generation;
  guint internal_preview_timeout;
  guint external_preview_timeout;
  GtkWidget *oof_external_row;
  AdwBanner *oof_error_banner;
  GtkWidget *oof_save;
  gboolean oof_busy;
  gboolean oof_loaded;

  StampAccount *account;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (StampPreferencesOutOfOffice, stamp_preferences_out_of_office, ADW_TYPE_NAVIGATION_PAGE);

static void
update_oof_controls (StampPreferencesOutOfOffice *self)
{
  gboolean scheduled = adw_combo_row_get_selected (self->oof_state) == STAMP_OUT_OF_OFFICE_STATE_SCHEDULED;
  gboolean external = adw_combo_row_get_selected (self->oof_audience) != 0;
  gboolean editable = self->oof_loaded && !self->oof_busy;
  gboolean valid = TRUE;
  gboolean internal_ready, external_ready;

  if (!self->oof_save)
    return;

  g_object_get (self->oof_internal, "ready", &internal_ready, NULL);
  g_object_get (self->oof_external, "ready", &external_ready, NULL);

  gtk_widget_set_visible (GTK_WIDGET (self->oof_start), scheduled);
  gtk_widget_set_visible (GTK_WIDGET (self->oof_end), scheduled);
  gtk_widget_set_visible (self->oof_external_row, external);

  gtk_widget_set_sensitive (GTK_WIDGET (self->oof_state), editable);
  gtk_widget_set_sensitive (GTK_WIDGET (self->oof_audience), editable);
  gtk_widget_set_sensitive (GTK_WIDGET (self->oof_start), editable);
  gtk_widget_set_sensitive (GTK_WIDGET (self->oof_end), editable);
  gtk_widget_set_sensitive (GTK_WIDGET (self->oof_internal), editable);
  gtk_widget_set_sensitive (GTK_WIDGET (self->oof_external), editable);

  if (!self->oof_busy) {
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->oof_save), _("Save"));
    gtk_widget_set_tooltip_text (self->oof_save, _("Save Automatic Replies"));
  }

  gtk_widget_remove_css_class (GTK_WIDGET (self->oof_start), "error");
  gtk_widget_remove_css_class (GTK_WIDGET (self->oof_end), "error");

  if (scheduled) {
    GDateTime *start = stamp_date_time_picker_get_date_time (self->oof_start);
    GDateTime *end = stamp_date_time_picker_get_date_time (self->oof_end);

    if (!start)
      gtk_widget_add_css_class (GTK_WIDGET (self->oof_start), "error");

    if (!end || (start && g_date_time_compare (start, end) >= 0))
      gtk_widget_add_css_class (GTK_WIDGET (self->oof_end), "error");

    valid = start && end && g_date_time_compare (start, end) < 0;
  }

  gtk_widget_set_sensitive (self->oof_save, editable && valid && internal_ready && external_ready);
}

static void
on_oof_selection_changed (GObject    *object,
                          GParamSpec *pspec,
                          gpointer    user_data)
{
  update_oof_controls (STAMP_PREFERENCES_OUT_OF_OFFICE (user_data));
}

static GWeakRef *
oof_weak_ref_new (StampPreferencesOutOfOffice *self)
{
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, self);
  return ref;
}

static void
oof_weak_ref_free (GWeakRef *ref)
{
  g_weak_ref_clear (ref);
  g_free (ref);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GWeakRef, oof_weak_ref_free)

typedef struct {
  GWeakRef page;
  gboolean internal;
  guint generation;
} OofPreviewData;

typedef struct {
  StampPreferencesOutOfOffice *self;
  gboolean internal;
} OofPreviewTimeout;

static void
on_oof_preview_ready (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  OofPreviewData *data = user_data;
  g_autoptr (StampPreferencesOutOfOffice) self = g_weak_ref_get (&data->page);
  g_autoptr (GError) error = NULL;
  g_autofree gchar *plain = NULL;
  g_autofree gchar *html = stamp_rich_text_editor_get_html_finish (STAMP_RICH_TEXT_EDITOR (source), result, &plain, &error);
  g_autoptr (GString) preview = g_string_new (NULL);
  gboolean space = FALSE;

  if (self && html && plain && data->generation == (data->internal ? self->internal_preview_generation : self->external_preview_generation)) {
    for (const gchar *p = plain; *p; p = g_utf8_next_char (p)) {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isspace (c)) {
        space = preview->len > 0;
      } else {
        if (space)
          g_string_append_c (preview, ' ');
        g_string_append_unichar (preview, c);
        space = FALSE;
      }
    }

    adw_action_row_set_subtitle (data->internal ? self->oof_internal_link : self->oof_external_link, preview->str);
  }

  g_weak_ref_clear (&data->page);
  g_free (data);
}

static void
request_oof_preview (StampPreferencesOutOfOffice *self,
                     StampRichTextEditor         *editor)
{
  OofPreviewData *data;

  data = g_new0 (OofPreviewData, 1);
  g_weak_ref_init (&data->page, self);
  data->internal = editor == self->oof_internal;
  data->generation = data->internal ? ++self->internal_preview_generation : ++self->external_preview_generation;
  stamp_rich_text_editor_get_html_async (editor, self->cancellable, on_oof_preview_ready, data);
}

static gboolean
on_oof_preview_timeout (gpointer user_data)
{
  OofPreviewTimeout *timeout = user_data;
  StampPreferencesOutOfOffice *self = timeout->self;

  if (timeout->internal) {
    self->internal_preview_timeout = 0;
    request_oof_preview (self, self->oof_internal);
  } else {
    self->external_preview_timeout = 0;
    request_oof_preview (self, self->oof_external);
  }

  return G_SOURCE_REMOVE;
}

static void
on_oof_editor_changed (StampRichTextEditor         *editor,
                       StampPreferencesOutOfOffice *self)
{
  gboolean internal = editor == self->oof_internal;
  guint *source_id = internal ? &self->internal_preview_timeout : &self->external_preview_timeout;
  OofPreviewTimeout *timeout;
  gboolean ready;

  g_object_get (editor, "ready", &ready, NULL);
  if (!ready)
    return;

  g_clear_handle_id (source_id, g_source_remove);

  timeout = g_new0 (OofPreviewTimeout, 1);
  timeout->self = self;
  timeout->internal = internal;
  *source_id = g_timeout_add_full (G_PRIORITY_DEFAULT, 300, on_oof_preview_timeout, timeout, g_free);
}

static void
on_oof_editor_ready (StampRichTextEditor         *editor,
                     GParamSpec                  *pspec,
                     StampPreferencesOutOfOffice *self)
{
  update_oof_controls (self);
  on_oof_editor_changed (editor, self);
}

static void
show_oof_error (StampPreferencesOutOfOffice *self,
                const gchar                 *title,
                const gchar                 *detail)
{
  g_autofree gchar *message = detail && *detail ? g_strdup_printf ("%s: %s", title, detail) : g_strdup (title);

  adw_banner_set_title (self->oof_error_banner, message);
  adw_banner_set_revealed (self->oof_error_banner, TRUE);
}

static void
on_oof_error_dismissed (AdwBanner *banner,
                        gpointer   user_data)
{
  adw_banner_set_revealed (banner, FALSE);
}

static void
on_oof_loaded (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr (GWeakRef) ref = user_data;
  g_autoptr (StampPreferencesOutOfOffice) self = g_weak_ref_get (ref);
  g_autoptr (GError) error = NULL;
  g_autoptr (StampOutOfOffice) settings = stamp_out_of_office_load_finish (STAMP_ACCOUNT (source), result, &error);

  if (!self)
    return;

  self->oof_busy = FALSE;
  if (!settings) {
    show_oof_error (self, _("Could Not Load Automatic Replies"), error ? error->message : NULL);
  } else {
    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    g_autoptr (GDateTime) tomorrow = g_date_time_add_days (now, 1);

    adw_combo_row_set_selected (self->oof_state, settings->state);
    adw_combo_row_set_selected (self->oof_audience, settings->audience);
    stamp_date_time_picker_set_date_time (self->oof_start, settings->start ? settings->start : now);
    stamp_date_time_picker_set_date_time (self->oof_end, settings->end ? settings->end : tomorrow);
    stamp_rich_text_editor_set_html (self->oof_internal, settings->internal_reply);
    stamp_rich_text_editor_set_html (self->oof_external, settings->external_reply);
    self->oof_loaded = TRUE;
    adw_banner_set_revealed (self->oof_error_banner, FALSE);
  }
  update_oof_controls (self);
}

static void
load_oof (StampPreferencesOutOfOffice *self)
{
  if (self->oof_busy)
    return;

  self->oof_busy = TRUE;
  adw_banner_set_revealed (self->oof_error_banner, FALSE);
  update_oof_controls (self);
  stamp_out_of_office_load (self->account, self->cancellable, on_oof_loaded, oof_weak_ref_new (self));
}

static void
on_oof_saved (GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
  g_autoptr (GWeakRef) ref = user_data;
  g_autoptr (StampPreferencesOutOfOffice) self = g_weak_ref_get (ref);
  g_autoptr (GError) error = NULL;
  gboolean success = stamp_out_of_office_save_finish (STAMP_ACCOUNT (source), result, &error);

  if (!self)
    return;

  self->oof_busy = FALSE;
  if (success)
    adw_banner_set_revealed (self->oof_error_banner, FALSE);
  else
    show_oof_error (self, _("Could Not Save Automatic Replies"), error ? error->message : NULL);

  update_oof_controls (self);
}

typedef struct {
  GWeakRef page;
  StampOutOfOffice *settings;
} OofSaveData;

static void
save_data_free (OofSaveData *data)
{
  g_weak_ref_clear (&data->page);
  stamp_out_of_office_free (data->settings);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OofSaveData, save_data_free)

static void
on_oof_html_ready (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  OofSaveData *data = user_data;
  g_autoptr (StampPreferencesOutOfOffice) self = g_weak_ref_get (&data->page);
  g_autoptr (GError) error = NULL;
  g_autofree gchar *html = stamp_rich_text_editor_get_html_finish (STAMP_RICH_TEXT_EDITOR (source), result, NULL, &error);

  if (!self) {
    save_data_free (data);
    return;
  }

  if (!html) {
    self->oof_busy = FALSE;
    show_oof_error (self, _("Could Not Save Automatic Replies"), error ? error->message : _("Could not read the reply HTML"));
    update_oof_controls (self);
    save_data_free (data);
    return;
  }

  if (source == G_OBJECT (self->oof_internal)) {
    data->settings->internal_reply = g_steal_pointer (&html);
    stamp_rich_text_editor_get_html_async (self->oof_external, self->cancellable, on_oof_html_ready, data);
    return;
  }

  data->settings->external_reply = g_steal_pointer (&html);
  stamp_out_of_office_save (self->account, g_steal_pointer (&data->settings), self->cancellable,
                            on_oof_saved, oof_weak_ref_new (self));
  save_data_free (data);
}

static void
on_oof_save_clicked (GtkWidget *button,
                     gpointer   user_data)
{
  StampPreferencesOutOfOffice *self = STAMP_PREFERENCES_OUT_OF_OFFICE (user_data);
  OofSaveData *data;
  GDateTime *start, *end;

  if (self->oof_busy || !self->oof_loaded || !gtk_widget_get_sensitive (self->oof_save))
    return;

  start = stamp_date_time_picker_get_date_time (self->oof_start);
  end = stamp_date_time_picker_get_date_time (self->oof_end);
  if (!start || !end)
    return;

  data = g_new0 (OofSaveData, 1);
  g_weak_ref_init (&data->page, self);
  data->settings = g_new0 (StampOutOfOffice, 1);
  data->settings->state = adw_combo_row_get_selected (self->oof_state);
  data->settings->audience = adw_combo_row_get_selected (self->oof_audience);
  data->settings->start = g_date_time_ref (start);
  data->settings->end = g_date_time_ref (end);

  self->oof_busy = TRUE;
  adw_banner_set_revealed (self->oof_error_banner, FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->oof_save), _("Saving…"));
  gtk_widget_set_tooltip_text (self->oof_save, _("Saving automatic replies…"));
  update_oof_controls (self);
  stamp_rich_text_editor_get_html_async (self->oof_internal, self->cancellable, on_oof_html_ready, data);
}

static void
stamp_preferences_out_of_office_dispose (GObject *object)
{
  StampPreferencesOutOfOffice *self = STAMP_PREFERENCES_OUT_OF_OFFICE (object);

  g_clear_handle_id (&self->internal_preview_timeout, g_source_remove);
  g_clear_handle_id (&self->external_preview_timeout, g_source_remove);
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->account);

  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_PREFERENCES_OUT_OF_OFFICE);

  G_OBJECT_CLASS (stamp_preferences_out_of_office_parent_class)->dispose (object);
}

static void
stamp_preferences_out_of_office_class_init (StampPreferencesOutOfOfficeClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = stamp_preferences_out_of_office_dispose;
  g_type_ensure (STAMP_TYPE_RICH_TEXT_EDITOR);
  g_type_ensure (STAMP_TYPE_DATE_TIME_PICKER);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/stamp-preferences-out-of-office.ui");
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_state);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_audience);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_start);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_end);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_internal);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_external);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_internal_link);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_external_link);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_external_row);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_error_banner);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesOutOfOffice, oof_save);

  gtk_widget_class_bind_template_callback (widget_class, on_oof_error_dismissed);
  gtk_widget_class_bind_template_callback (widget_class, on_oof_save_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_oof_selection_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_oof_editor_ready);
  gtk_widget_class_bind_template_callback (widget_class, on_oof_editor_changed);
}

static void
stamp_preferences_out_of_office_init (StampPreferencesOutOfOffice *self)
{
  self->cancellable = g_cancellable_new ();

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_accessible_update_property (GTK_ACCESSIBLE (self->oof_internal), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Reply to People in Your Organization"), -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->oof_external), GTK_ACCESSIBLE_PROPERTY_LABEL, _("External Reply"), -1);

  update_oof_controls (self);
}

GtkWidget *
stamp_preferences_out_of_office_new (StampAccount *account)
{
  StampPreferencesOutOfOffice *self = g_object_new (STAMP_TYPE_PREFERENCES_OUT_OF_OFFICE, NULL);

  self->account = g_object_ref (account);
  load_oof (self);

  return GTK_WIDGET (self);
}
