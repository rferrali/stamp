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

#include "stamp-folder-row.h"

#include <adwaita.h>

#include "stamp-account-item.h"
#include "stamp-folder-item.h"

struct _StampFolderRow {
  GtkBox parent_instance;

  GtkImage *image;
  GtkInscription *inscription;
  GtkLabel *unread;
  AdwSpinner *spinner;
  GtkImage *error;
};

G_DEFINE_FINAL_TYPE (StampFolderRow, stamp_folder_row, GTK_TYPE_BOX);

static void
stamp_folder_row_init (StampFolderRow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

static void
stamp_folder_row_class_init (StampFolderRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/folder-list/stamp-folder-row.ui");

  gtk_widget_class_bind_template_child (widget_class, StampFolderRow, image);
  gtk_widget_class_bind_template_child (widget_class, StampFolderRow, inscription);
  gtk_widget_class_bind_template_child (widget_class, StampFolderRow, unread);
  gtk_widget_class_bind_template_child (widget_class, StampFolderRow, spinner);
  gtk_widget_class_bind_template_child (widget_class, StampFolderRow, error);
}

GtkWidget *
stamp_folder_row_new (void)
{
  return g_object_new (STAMP_TYPE_FOLDER_ROW, NULL);
}

static gboolean
transform_unread_to (GBinding     *binding,
                     const GValue *from_value,
                     GValue       *to_value,
                     gpointer      user_data)
{
  StampFolderRow *self = STAMP_FOLDER_ROW (user_data);
  guint unread = g_value_get_uint (from_value);

  gtk_widget_set_visible (GTK_WIDGET (self->unread), unread > 0);
  if (unread) {
    g_autofree char *label = g_strdup_printf ("%u", unread);
    g_autofree char *description = g_strdup_printf (g_dngettext (NULL, "%u Unread Message", "%u Unread Messages", unread), unread);

    gtk_accessible_update_property (GTK_ACCESSIBLE (self->unread), GTK_ACCESSIBLE_PROPERTY_LABEL, description, -1);
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->unread), description);
    g_value_set_string (to_value, label);
  }

  return TRUE;
}

static gboolean
transform_error_to (GBinding     *binding,
                    const GValue *from_value,
                    GValue       *to_value,
                    gpointer      user_data)
{
  StampFolderRow *self = STAMP_FOLDER_ROW (user_data);
  GError *error = g_value_get_pointer (from_value);

  if (error)
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->error), error->message);

  g_value_set_boolean (to_value, error != NULL);

  return TRUE;
}

void
stamp_folder_row_bind (StampFolderRow *self,
                       StampItem      *item)
{
  const gchar *icon_name = stamp_item_get_icon_name (item);

  gtk_image_set_from_icon_name (self->image, icon_name);
  gtk_widget_set_visible (GTK_WIDGET (self->image), icon_name != NULL);

  g_object_bind_property (item, "name", self->inscription, "text", G_BINDING_SYNC_CREATE);
  g_object_bind_property (item, "name", self->inscription, "tooltip-text", G_BINDING_SYNC_CREATE);
  g_object_bind_property (item, "loading", self->spinner, "visible", G_BINDING_SYNC_CREATE);
  g_object_bind_property_full (item, "error", self->error, "visible", G_BINDING_SYNC_CREATE, transform_error_to, NULL, g_object_ref (self), g_object_unref);

  if (STAMP_IS_ACCOUNT_ITEM (item)) {
    gtk_widget_add_css_class (GTK_WIDGET (self), "bold");
    gtk_widget_set_visible (GTK_WIDGET (self->unread), FALSE);
  } else {
    if (g_object_class_find_property (G_OBJECT_GET_CLASS (item), "unread"))
      g_object_bind_property_full (item, "unread", self->unread, "label", G_BINDING_SYNC_CREATE, transform_unread_to, NULL, g_object_ref (self), g_object_unref);
    gtk_widget_remove_css_class (GTK_WIDGET (self), "bold");
  }
}
