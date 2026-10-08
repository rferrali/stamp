/*
 * Copyright 2025-2026 Jan-Michael Brummer
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

#include "stamp-attachment-button.h"

#include <adwaita.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

struct _StampAttachmentButton {
  GtkBox parent_instance;

  CamelMimePart *mime_part;
  GFile *file;
  gchar *filename;
  gchar *content_type;
  gsize size;
  GBytes *data;
  GCancellable *cancellable;
  gboolean editable;

  GtkWidget *image;
  GtkWidget *filename_label;
  GtkWidget *size_label;
  GtkWidget *context_menu;
};

G_DEFINE_FINAL_TYPE (StampAttachmentButton, stamp_attachment_button, GTK_TYPE_BOX);

typedef enum {
  PROP_MIME_PART = 1,
  PROP_FILE,
  PROP_FILENAME,
  PROP_CONTENT_TYPE,
  PROP_SIZE,
  PROP_DATA,
  PROP_EDITABLE,
} StampAttachmentButtonProps;

static GParamSpec *properties[PROP_EDITABLE + 1];

static void
stamp_attachment_button_get_property (GObject    *object,
                                      guint       property_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (object);

  switch ((StampAttachmentButtonProps)property_id) {
    case PROP_MIME_PART:
      g_value_set_object (value, self->mime_part);
      break;
    case PROP_FILE:
      g_value_set_object (value, self->file);
      break;
    case PROP_FILENAME:
      g_value_set_string (value, self->filename);
      break;
    case PROP_CONTENT_TYPE:
      g_value_set_string (value, self->content_type);
      break;
    case PROP_SIZE:
      g_value_set_uint64 (value, self->size);
      break;
    case PROP_DATA:
      g_value_set_boxed (value, self->data);
      break;
    case PROP_EDITABLE:
      g_value_set_boolean (value, self->editable);
      break;
  }
}

static void
stamp_attachment_button_set_property (GObject      *object,
                                      guint         property_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (object);

  switch ((StampAttachmentButtonProps)property_id) {
    case PROP_MIME_PART:
      self->mime_part = g_value_get_object (value);
      break;
    case PROP_FILE:
      self->file = g_value_get_object (value);
      if (self->file)
        g_object_ref (self->file);
      break;
    case PROP_FILENAME:
      g_free (self->filename);
      self->filename = g_value_dup_string (value);
      break;
    case PROP_CONTENT_TYPE:
      g_free (self->content_type);
      self->content_type = g_value_dup_string (value);
      break;
    case PROP_SIZE:
      self->size = g_value_get_uint64 (value);
      break;
    case PROP_DATA:
      g_bytes_unref (self->data);
      self->data = g_value_get_boxed (value);
      if (self->data)
        g_bytes_ref (self->data);
      break;
    case PROP_EDITABLE:
      self->editable = g_value_get_boolean (value);
      break;
  }
}

static void
on_file_launched (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  gtk_file_launcher_launch_finish (GTK_FILE_LAUNCHER (source), res, &error);
  if (error)
    g_warning ("%s: %s (domain=%s, code=%d)", G_STRFUNC, error->message, g_quark_to_string (error->domain), error->code);
}

static void
response_open_data_cb (AdwAlertDialog *dialog,
                       gchar          *response,
                       gpointer        user_data)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileLauncher) launcher = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileIOStream) stream = NULL;
  g_autofree char *tmp = g_strdup_printf ("XXXXXX-%s", self->filename);

  file = g_file_new_tmp (tmp, &stream, &error);
  if (error) {
    g_warning ("%s: Could not create temporary file: %s", G_STRFUNC, error->message);
    return;
  }

  g_output_stream_write (g_io_stream_get_output_stream (G_IO_STREAM (stream)),
                         g_bytes_get_data (self->data, NULL),
                         g_bytes_get_size (self->data),
                         NULL,
                         &error);
  if (error) {
    g_warning ("%s: Could not write data to stream: %s", G_STRFUNC, error->message);
    return;
  }

  launcher = gtk_file_launcher_new (file);

  gtk_file_launcher_launch (launcher, GTK_WINDOW (root), NULL, on_file_launched, NULL);
}

static void
response_open_cb (AdwAlertDialog *dialog,
                  gchar          *response,
                  gpointer        user_data)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (GtkFileLauncher) launcher = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileIOStream) stream = NULL;
  g_autofree char *tmp = g_strdup_printf ("XXXXXX-%s", self->filename);

  file = g_file_new_tmp (tmp, &stream, &error);
  if (error) {
    g_warning ("%s: Could not create temporary file: %s", G_STRFUNC, error->message);
    return;
  }

  camel_data_wrapper_decode_to_output_stream_sync (CAMEL_DATA_WRAPPER (camel_medium_get_content (CAMEL_MEDIUM (self->mime_part))),
                                                   g_io_stream_get_output_stream (G_IO_STREAM (stream)),
                                                   NULL,
                                                   &error);
  if (error) {
    g_warning ("%s: Could not decode mime to stream: %s", G_STRFUNC, error->message);
    return;
  }

  launcher = gtk_file_launcher_new (file);

  gtk_file_launcher_launch (launcher, GTK_WINDOW (root), NULL, on_file_launched, NULL);
}

static void
on_open_activate (GAction  *action,
                  GVariant *parameter,
                  gpointer  user_data)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  if (self->mime_part) {
    g_autofree char *tmp = g_strdup_printf (_("Trust and open `%s`?"), gtk_label_get_text (GTK_LABEL (self->filename_label)));
    AdwDialog *dialog = adw_alert_dialog_new (tmp,
                                              _("Attachment may cause damage to your system if opened. Only open files from trusted sources."));

    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "open", _("Open Anyway"));
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "open");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    g_signal_connect_object (dialog, "response::open", G_CALLBACK (response_open_cb), self, G_CONNECT_DEFAULT);

    adw_dialog_present (dialog, GTK_WIDGET (root));
  } else if (self->data) {
    g_autofree char *tmp = g_strdup_printf (_("Trust and open `%s`?"), gtk_label_get_text (GTK_LABEL (self->filename_label)));
    AdwDialog *dialog = adw_alert_dialog_new (tmp,
                                              _("Attachment may cause damage to your system if opened. Only open files from trusted sources."));

    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "open", _("Open Anyway"));
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "open");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    g_signal_connect_object (dialog, "response::open", G_CALLBACK (response_open_data_cb), self, G_CONNECT_DEFAULT);

    adw_dialog_present (dialog, GTK_WIDGET (root));
  } else if (self->file) {
    g_autoptr (GtkFileLauncher) launcher = NULL;

    launcher = gtk_file_launcher_new (self->file);
    gtk_file_launcher_launch (launcher, GTK_WINDOW (root), NULL, on_file_launched, NULL);
  }
}

static void
on_open_folder (AdwToast *toast,
                gpointer  user_data)
{
  GFile *file = G_FILE (user_data);
  GtkWindow *window = g_object_get_data (G_OBJECT (toast), "open-folder-window");
  g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (file);

  gtk_file_launcher_open_containing_folder (launcher, window, NULL, NULL, NULL);
}

static void
on_save_as (GObject      *source_object,
            GAsyncResult *res,
            gpointer      user_data)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (user_data);
  g_autoptr (GtkFileDialog) dialog = GTK_FILE_DIALOG (source_object);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GFileOutputStream) file_output_stream = NULL;
  g_autoptr (GFileIOStream) file_io_stream = NULL;
  GOutputStream *stream = NULL;
  AdwToastOverlay *overlay;

  file = gtk_file_dialog_save_finish (dialog, res, &error);
  if (error) {
    g_warning ("Could not save attachment file: %s", error->message);
    return;
  }

  file_output_stream = g_file_create (file, G_FILE_CREATE_NONE, NULL, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
      g_warning ("Could not create file: %s", error->message);
      return;
    }

    g_clear_error (&error);
    file_io_stream = g_file_open_readwrite (file, NULL, &error);
    if (error) {
      g_warning ("Could not open file: %s", error->message);
      return;
    }

    stream = g_io_stream_get_output_stream (G_IO_STREAM (file_io_stream));
  } else {
    stream = G_OUTPUT_STREAM (file_output_stream);
  }

  if (self->mime_part) {
    camel_data_wrapper_decode_to_output_stream_sync (CAMEL_DATA_WRAPPER (camel_medium_get_content (CAMEL_MEDIUM (self->mime_part))),
                                                     stream,
                                                     NULL,
                                                     &error);
  } else if (self->data) {
    g_output_stream_write_all (stream, g_bytes_get_data (self->data, NULL), g_bytes_get_size (self->data), NULL, NULL, &error);
  }

  if (error) {
    g_warning ("Could not write file, abort: %s", error->message);
    return;
  }

  overlay = ADW_TOAST_OVERLAY (gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TOAST_OVERLAY));
  if (overlay) {
    AdwToast *toast = adw_toast_new (_("Attachment Saved"));

    adw_toast_set_button_label (toast, _("Open Folder"));
    g_object_set_data (G_OBJECT (toast), "open-folder-window", GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))));
    g_signal_connect_data (toast, "button-clicked", G_CALLBACK (on_open_folder), g_object_ref (file), (GClosureNotify)g_object_unref, G_CONNECT_DEFAULT);
    adw_toast_overlay_add_toast (overlay, toast);
  }
}

static void
on_save_as_activate (GAction  *action,
                     GVariant *parameter,
                     gpointer  user_data)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  g_autofree char *tmp = g_strdup_printf (_("Save `%s`?"), gtk_label_get_text (GTK_LABEL (self->filename_label)));

  gtk_file_dialog_set_accept_label (dialog, _("Save"));
  gtk_file_dialog_set_initial_name (dialog, gtk_label_get_text (GTK_LABEL (self->filename_label)));
  gtk_file_dialog_set_title (dialog, tmp);

  gtk_file_dialog_save (dialog, GTK_WINDOW (root), NULL, on_save_as, self);
}

static void
on_rename_dialog_response (AdwAlertDialog *dialog,
                           gchar          *response,
                           gpointer        user_data)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (user_data);

  if (g_strcmp0 (response, "rename") == 0) {
    GtkEntry *entry = g_object_get_data (G_OBJECT (dialog), "rename-entry");
    const gchar *new_name = gtk_editable_get_text (GTK_EDITABLE (entry));

    if (new_name && *new_name) {
      g_set_str (&self->filename, new_name);

      gtk_label_set_text (GTK_LABEL (self->filename_label), new_name);
      gtk_widget_set_tooltip_text (GTK_WIDGET (self), new_name);
    }
  }
}

static void
on_rename_activate (GAction  *action,
                    GVariant *parameter,
                    gpointer  user_data)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  AdwDialog *dialog = adw_alert_dialog_new (_("Rename Attachment"), _("Enter a new name for the attachment:"));
  GtkWidget *entry = gtk_entry_new ();

  gtk_editable_set_text (GTK_EDITABLE (entry), stamp_attachment_button_get_filename (self));
  gtk_editable_set_position (GTK_EDITABLE (entry), -1);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);

  g_object_set_data (G_OBJECT (dialog), "rename-entry", entry);

  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "rename", _("Rename"));
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "rename");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "rename", ADW_RESPONSE_SUGGESTED);

  g_signal_connect_object (dialog, "response", G_CALLBACK (on_rename_dialog_response), self, G_CONNECT_DEFAULT);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (root));
  gtk_widget_grab_focus (entry);
}

static void
on_remove_activate (GAction  *action,
                    GVariant *parameter,
                    gpointer  user_data)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (user_data);

  gtk_widget_unparent (GTK_WIDGET (self));
}

static gboolean
remove_menu_item (GMenu       *menu,
                  const gchar *action_name)
{
  gint i, n;

  n = g_menu_model_get_n_items (G_MENU_MODEL (menu));

  for (i = 0; i < n; i++) {
    g_autofree char *item_action = NULL;
    g_autofree char *submenu_id = NULL;
    g_autoptr (GMenuModel) section = NULL;

    g_menu_model_get_item_attribute (G_MENU_MODEL (menu),
                                     i,
                                     G_MENU_ATTRIBUTE_ACTION,
                                     "s",
                                     &item_action);

    if (g_strcmp0 (action_name, item_action) == 0) {
      g_menu_remove (menu, i);
      return TRUE;
    }
  }

  return FALSE;
}

static void
stamp_attachment_button_init (StampAttachmentButton *self)
{
  GSimpleActionGroup *actions = g_simple_action_group_new ();
  GSimpleAction *open_action = g_simple_action_new ("open", NULL);
  GSimpleAction *save_as_action = g_simple_action_new ("save-as", NULL);
  GSimpleAction *remove_action = g_simple_action_new ("remove", NULL);
  GSimpleAction *rename_action = g_simple_action_new ("rename", NULL);

  g_action_map_add_action (G_ACTION_MAP (actions), G_ACTION (open_action));
  g_signal_connect_object (open_action, "activate", G_CALLBACK (on_open_activate), self, G_CONNECT_DEFAULT);

  g_signal_connect_object (save_as_action, "activate", G_CALLBACK (on_save_as_activate), self, G_CONNECT_DEFAULT);
  g_action_map_add_action (G_ACTION_MAP (actions), G_ACTION (save_as_action));

  g_signal_connect_object (remove_action, "activate", G_CALLBACK (on_remove_activate), self, G_CONNECT_DEFAULT);
  g_action_map_add_action (G_ACTION_MAP (actions), G_ACTION (remove_action));

  g_signal_connect_object (rename_action, "activate", G_CALLBACK (on_rename_activate), self, G_CONNECT_DEFAULT);
  g_action_map_add_action (G_ACTION_MAP (actions), G_ACTION (rename_action));

  gtk_widget_insert_action_group (GTK_WIDGET (self), "attachmentbutton", G_ACTION_GROUP (actions));
}

static void
stamp_attachment_button_constructed (GObject *object)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (object);
  g_autofree char *mime_type = NULL;
  g_autofree char *glib_type = NULL;
  g_autoptr (GIcon) content_icon = NULL;
  g_autofree char *tmp = NULL;
  g_autofree char *readable_size = NULL;
  const gchar *filename = NULL;
  gsize size = 0;

  G_OBJECT_CLASS (stamp_attachment_button_parent_class)->constructed (object);

  self->cancellable = g_cancellable_new ();
  gtk_widget_init_template (GTK_WIDGET (self));

  if (self->mime_part) {
    mime_type = camel_content_type_simple (camel_mime_part_get_content_type (self->mime_part));
    size = camel_data_wrapper_calculate_decoded_size_sync (CAMEL_DATA_WRAPPER (self->mime_part), self->cancellable, NULL);
    filename = camel_mime_part_get_filename (self->mime_part);
    glib_type = g_content_type_from_mime_type (mime_type);
    content_icon = g_content_type_get_icon (glib_type);
  } else if (self->file) {
    GFileInfo *info = g_file_query_info (self->file,
                                         G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                         G_FILE_QUERY_INFO_NONE, NULL, NULL);

    filename = g_file_info_get_display_name (info);
    size = g_file_info_get_size (info);
    content_icon = g_content_type_get_icon (g_file_info_get_content_type (info));
  } else if (self->filename && self->content_type) {
    mime_type = g_strdup (self->content_type);
    glib_type = g_content_type_from_mime_type (mime_type);
    if (glib_type)
      content_icon = g_content_type_get_icon (glib_type);
    filename = self->filename;
    size = self->size;
  } else {
    g_warning ("%s: Unhandled code, abort", G_STRFUNC);
    return;
  }

  if (content_icon)
    gtk_image_set_from_gicon (GTK_IMAGE (self->image), content_icon);
  if (filename) {
    gtk_label_set_text (GTK_LABEL (self->filename_label), filename);
    gtk_widget_set_tooltip_text (GTK_WIDGET (self), filename);
  }

  if (size > 0) {
    readable_size = g_format_size (size);
    tmp = g_strdup_printf ("<small>%s</small>", readable_size);
    gtk_label_set_markup (GTK_LABEL (self->size_label), tmp);
  }

  if (!self->editable) {
    remove_menu_item (G_MENU (self->context_menu), "attachmentbutton.remove");
    remove_menu_item (G_MENU (self->context_menu), "attachmentbutton.rename");
  } else {
    remove_menu_item (G_MENU (self->context_menu), "attachmentbutton.save-as");
  }
}

static void
stamp_attachment_button_dispose (GObject *object)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (object);

  g_clear_object (&self->mime_part);
  g_clear_object (&self->file);
  g_clear_pointer (&self->filename, g_free);
  g_clear_pointer (&self->content_type, g_free);
  g_bytes_unref (self->data);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (stamp_attachment_button_parent_class)->dispose (object);
}

static void
on_attachment_clicked (GtkWidget *button,
                       gpointer   user_data)
{
  StampAttachmentButton *self = STAMP_ATTACHMENT_BUTTON (user_data);
  stamp_attachment_button_activate (self);
}

static void
stamp_attachment_button_class_init (StampAttachmentButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/message-list/stamp-attachment-button.ui");

  object_class->get_property = stamp_attachment_button_get_property;
  object_class->set_property = stamp_attachment_button_set_property;
  object_class->constructed = stamp_attachment_button_constructed;
  object_class->dispose = stamp_attachment_button_dispose;

  gtk_widget_class_bind_template_child (widget_class, StampAttachmentButton, image);
  gtk_widget_class_bind_template_child (widget_class, StampAttachmentButton, filename_label);
  gtk_widget_class_bind_template_child (widget_class, StampAttachmentButton, size_label);
  gtk_widget_class_bind_template_child (widget_class, StampAttachmentButton, context_menu);

  gtk_widget_class_bind_template_callback (widget_class, on_attachment_clicked);

  properties[PROP_MIME_PART] = g_param_spec_object ("mime-part",
                                                    NULL,
                                                    NULL,
                                                    CAMEL_TYPE_MIME_PART,
                                                    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_FILE] =
    g_param_spec_object ("file",
                         NULL,
                         NULL,
                         G_TYPE_FILE,
                         G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_FILENAME] =
    g_param_spec_string ("filename",
                         NULL,
                         NULL,
                         NULL,
                         G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_CONTENT_TYPE] =
    g_param_spec_string ("content-type",
                         NULL,
                         NULL,
                         NULL,
                         G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_SIZE] =
    g_param_spec_uint64 ("size",
                         NULL,
                         NULL,
                         0, G_MAXUINT64, 0,
                         G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_DATA] =
    g_param_spec_boxed ("data",
                        NULL,
                        NULL,
                        G_TYPE_BYTES,
                        G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_EDITABLE] =
    g_param_spec_boolean ("editable",
                          NULL,
                          NULL,
                          FALSE,
                          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);
}

GtkWidget *
stamp_attachment_button_new (CamelMimePart *mime_part)
{
  return g_object_new (STAMP_TYPE_ATTACHMENT_BUTTON, "mime-part", mime_part, NULL);
}

GtkWidget *
stamp_attachment_button_new_from_data (const gchar *filename,
                                       const gchar *content_type,
                                       gsize        size,
                                       GBytes      *data,
                                       gboolean     editable)
{
  return g_object_new (STAMP_TYPE_ATTACHMENT_BUTTON,
                       "filename", filename,
                       "content-type", content_type,
                       "size", size,
                       "data", data,
                       "editable", editable,
                       NULL);
}

GtkWidget *
stamp_attachment_button_new_from_file (GFile *file)
{
  return g_object_new (STAMP_TYPE_ATTACHMENT_BUTTON, "file", file,
                       "editable", TRUE, NULL);
}

void
stamp_attachment_button_activate (StampAttachmentButton *self)
{
  on_open_activate (NULL, NULL, self);
}

const gchar *
stamp_attachment_button_get_filename (StampAttachmentButton *self)
{
  return gtk_label_get_text (GTK_LABEL (self->filename_label));
}

gboolean
stamp_attachment_button_save_to_file (StampAttachmentButton  *self,
                                      GFile                  *file,
                                      GError                **error)
{
  if (self->mime_part) {
    g_autoptr (GFileOutputStream) stream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
    if (!stream)
      return FALSE;

    return camel_data_wrapper_decode_to_output_stream_sync (CAMEL_DATA_WRAPPER (camel_medium_get_content (CAMEL_MEDIUM (self->mime_part))), G_OUTPUT_STREAM (stream), NULL, error);
  }

  if (self->data) {
    return g_file_replace_contents (file, g_bytes_get_data (self->data, NULL), g_bytes_get_size (self->data), NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, error);
  }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Attachment has no data to save");

  return FALSE;
}

CamelMimePart *
stamp_attachment_button_get_mime_part (StampAttachmentButton *self)
{
  g_autoptr (GFileInfo) info = NULL;
  const gchar *content_type;
  const gchar *mime_type;
  CamelMimePart *part = NULL;
  CamelDataWrapper *wrapper;
  GInputStream *input_stream;

  if (!self->file)
    return NULL;

  info = g_file_query_info (self->file,
                            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);

  content_type = g_file_info_get_content_type (info);
  mime_type = g_content_type_get_mime_type (content_type);

  wrapper = camel_data_wrapper_new ();

  input_stream = G_INPUT_STREAM (g_file_read (self->file, NULL, NULL));
  camel_data_wrapper_construct_from_input_stream_sync (wrapper, input_stream, self->cancellable, NULL);

  camel_data_wrapper_set_mime_type (wrapper, mime_type);

  part = camel_mime_part_new ();
  camel_mime_part_set_disposition (part, "attachment");
  camel_mime_part_set_filename (part, self->filename ? self->filename : g_file_info_get_display_name (info));
  camel_medium_set_content (CAMEL_MEDIUM (part), wrapper);

  camel_mime_part_set_encoding (part, CAMEL_TRANSFER_ENCODING_BASE64);

  return part;
}
