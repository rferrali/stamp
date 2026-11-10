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

#include "stamp-preferences-signatures.h"

#include <glib/gi18n.h>

#include "stamp-rich-text-editor.h"
#include "stamp-session.h"
#include "stamp-signature.h"

struct _StampPreferencesSignatures {
  AdwNavigationPage parent_instance;

  AdwPreferencesGroup *signatures_group;
  AdwButtonRow *add_signature;

  GPtrArray *signature_rows;
};

G_DEFINE_FINAL_TYPE (StampPreferencesSignatures, stamp_preferences_signatures, ADW_TYPE_NAVIGATION_PAGE);

static void
on_signature_activated (AdwActionRow *row,
                        gpointer      user_data)
{
  StampPreferencesSignatures *self = STAMP_PREFERENCES_SIGNATURES (user_data);
  StampSignature *signature = g_object_get_data (G_OBJECT (row), "signature");
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (parent), ADW_NAVIGATION_PAGE (stamp_preferences_signature_editor_new (signature)));
}

static void
on_add_signature_clicked (GtkWidget *button,
                          gpointer   user_data)
{
  StampPreferencesSignatures *self = STAMP_PREFERENCES_SIGNATURES (user_data);
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (parent), ADW_NAVIGATION_PAGE (stamp_preferences_signature_editor_new (NULL)));
}

static void
on_signature_edit_clicked (GtkWidget *button,
                           gpointer   user_data)
{
  StampPreferencesSignatures *self = STAMP_PREFERENCES_SIGNATURES (user_data);
  AdwActionRow *row = ADW_ACTION_ROW (g_object_get_data (G_OBJECT (button), "row"));
  StampSignature *signature = g_object_get_data (G_OBJECT (row), "signature");
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (parent), ADW_NAVIGATION_PAGE (stamp_preferences_signature_editor_new (signature)));
}

static void
init_signatures (StampPreferencesSignatures *self)
{
  GList *signatures;
  guint i;

  for (i = 0; i < self->signature_rows->len; i++)
    adw_preferences_group_remove (self->signatures_group, g_ptr_array_index (self->signature_rows, i));

  g_ptr_array_set_size (self->signature_rows, 0);

  adw_preferences_group_remove (self->signatures_group, GTK_WIDGET (self->add_signature));

  signatures = stamp_session_get_signatures (stamp_session_get_default ());
  for (GList *iter = signatures; iter && iter->data; iter = g_list_next (iter)) {
    StampSignature *signature = (StampSignature *)iter->data;
    GtkWidget *row = adw_action_row_new ();
    GtkWidget *edit_button = gtk_button_new_from_icon_name ("document-edit-symbolic");

    gtk_button_set_has_frame (GTK_BUTTON (edit_button), FALSE);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

    g_object_set_data_full (G_OBJECT (row), "signature", signature, NULL);
    g_object_set_data (G_OBJECT (edit_button), "row", row);
    g_signal_connect_object (row, "activated", G_CALLBACK (on_signature_activated), self, G_CONNECT_DEFAULT);
    g_signal_connect_object (edit_button, "clicked", G_CALLBACK (on_signature_edit_clicked), self, G_CONNECT_DEFAULT);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), stamp_signature_get_name (signature));
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), edit_button);

    adw_preferences_group_add (self->signatures_group, row);
    g_ptr_array_add (self->signature_rows, row);
  }

  adw_preferences_group_add (self->signatures_group, GTK_WIDGET (self->add_signature));
}

static void
stamp_preferences_signatures_dispose (GObject *object)
{
  StampPreferencesSignatures *self = STAMP_PREFERENCES_SIGNATURES (object);

  g_clear_pointer (&self->signature_rows, g_ptr_array_unref);

  G_OBJECT_CLASS (stamp_preferences_signatures_parent_class)->dispose (object);
}

void
stamp_preferences_signatures_class_init (StampPreferencesSignaturesClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = stamp_preferences_signatures_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/stamp-preferences-signatures.ui");

  gtk_widget_class_bind_template_child (widget_class, StampPreferencesSignatures, signatures_group);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesSignatures, add_signature);

  gtk_widget_class_bind_template_callback (widget_class, on_add_signature_clicked);
}

void
stamp_preferences_signatures_init (StampPreferencesSignatures *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->signature_rows = g_ptr_array_new ();

  g_signal_connect_swapped (self->signatures_group, "map", G_CALLBACK (init_signatures), self);
}

GtkWidget *
stamp_preferences_signatures_new (void)
{
  return g_object_new (STAMP_TYPE_PREFERENCES_SIGNATURES, NULL);
}

struct _StampPreferencesSignatureEditor {
  AdwNavigationPage parent_instance;

  AdwWindowTitle *editor_window_title;
  AdwEntryRow *name_row;
  StampRichTextEditor *editor;
  AdwButtonRow *save;
  AdwButtonRow *remove;

  StampSignature *signature;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (StampPreferencesSignatureEditor, stamp_preferences_signature_editor, ADW_TYPE_NAVIGATION_PAGE);

static void
on_editor_get_body_html (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  g_autoptr (StampPreferencesSignatureEditor) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *html = NULL;
  const gchar *name;
  StampSession *session;

  html = stamp_rich_text_editor_get_html_finish (self->editor, res, NULL, &error);
  if (error) {
    g_warning ("%s: Could not load body html: %s", G_STRFUNC, error->message);
    return;
  }

  name = gtk_editable_get_text (GTK_EDITABLE (self->name_row));
  if (!name || strlen (name) == 0) {
    g_warning ("%s: Name is empty", G_STRFUNC);
    return;
  }

  session = stamp_session_get_default ();

  if (self->signature) {
    ESourceRegistry *registry = stamp_session_get_registry (session);

    stamp_signature_set_name (self->signature, name);
    e_source_set_display_name (stamp_signature_get_source (self->signature), name);
    stamp_signature_save (self->signature, html);

    e_source_registry_commit_source_sync (registry, stamp_signature_get_source (self->signature), NULL, &error);
    if (error)
      g_warning ("%s: Could not commit signature: %s", G_STRFUNC, error->message);
  } else {
    self->signature = stamp_session_create_signature (session, name, html, "text/html");
  }

  {
    GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));

    g_assert (ADW_IS_NAVIGATION_VIEW (parent));
    adw_navigation_view_pop (ADW_NAVIGATION_VIEW (parent));
  }
}

static void
on_editor_save_clicked (GtkWidget *button,
                        gpointer   user_data)
{
  StampPreferencesSignatureEditor *self = STAMP_PREFERENCES_SIGNATURE_EDITOR (user_data);

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  self->cancellable = g_cancellable_new ();

  stamp_rich_text_editor_get_html_async (self->editor, self->cancellable, on_editor_get_body_html, g_object_ref (self));
}

static void
on_editor_remove_clicked (GtkWidget *button,
                          gpointer   user_data)
{
  StampPreferencesSignatureEditor *self = STAMP_PREFERENCES_SIGNATURE_EDITOR (user_data);
  GtkWidget *parent;

  if (!self->signature)
    return;

  stamp_session_remove_signature (stamp_session_get_default (), self->signature);
  self->signature = NULL;

  parent = gtk_widget_get_parent (GTK_WIDGET (self));
  g_assert (ADW_IS_NAVIGATION_VIEW (parent));
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (parent));
}

static void
stamp_preferences_signature_editor_dispose (GObject *object)
{
  StampPreferencesSignatureEditor *self = STAMP_PREFERENCES_SIGNATURE_EDITOR (object);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (stamp_preferences_signature_editor_parent_class)->dispose (object);
}

void
stamp_preferences_signature_editor_class_init (StampPreferencesSignatureEditorClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = stamp_preferences_signature_editor_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/stamp-preferences-signatures-editor.ui");

  gtk_widget_class_bind_template_child (widget_class, StampPreferencesSignatureEditor, editor_window_title);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesSignatureEditor, name_row);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesSignatureEditor, editor);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesSignatureEditor, save);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesSignatureEditor, remove);

  gtk_widget_class_bind_template_callback (widget_class, on_editor_save_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_editor_remove_clicked);
}

static void
stamp_preferences_signature_editor_setup (StampPreferencesSignatureEditor *self)
{
  if (self->signature) {
    adw_window_title_set_title (self->editor_window_title, _("Edit Signature"));
    gtk_editable_set_text (GTK_EDITABLE (self->name_row), stamp_signature_get_name (self->signature));

    if (g_strcmp0 (stamp_signature_get_mime_type (self->signature), "text/html") == 0)
      stamp_rich_text_editor_set_html (self->editor, stamp_signature_get_content (self->signature));
    else
      stamp_rich_text_editor_set_plain_text (self->editor, stamp_signature_get_content (self->signature));
  } else {
    adw_window_title_set_title (self->editor_window_title, _("New Signature"));
  }

  gtk_widget_set_visible (GTK_WIDGET (self->save), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->remove), self->signature != NULL);
}

void
stamp_preferences_signature_editor_init (StampPreferencesSignatureEditor *self)
{
  g_type_ensure (STAMP_TYPE_RICH_TEXT_EDITOR);
  gtk_widget_init_template (GTK_WIDGET (self));
  g_object_bind_property (self->editor, "ready", self->save, "sensitive", G_BINDING_SYNC_CREATE);
}

GtkWidget *
stamp_preferences_signature_editor_new (StampSignature *signature)
{
  StampPreferencesSignatureEditor *self = g_object_new (STAMP_TYPE_PREFERENCES_SIGNATURE_EDITOR, NULL);

  self->signature = signature;

  stamp_preferences_signature_editor_setup (self);

  return GTK_WIDGET (self);
}
