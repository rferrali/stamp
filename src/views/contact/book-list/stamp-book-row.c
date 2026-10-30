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

#include "stamp-book-row.h"
#include "stamp-book-account-item.h"

struct _StampBookRow {
  GtkBox parent_instance;

  GtkWidget *image;
  GtkWidget *inscription;
  GtkWidget *spinner;
  GBinding *loading_binding;
};

G_DEFINE_FINAL_TYPE (StampBookRow, stamp_book_row, GTK_TYPE_BOX);

static void
stamp_book_row_init (StampBookRow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

static void
stamp_book_row_dispose (GObject *object)
{
  stamp_book_row_unbind (STAMP_BOOK_ROW (object));
  gtk_widget_dispose_template (GTK_WIDGET (object), STAMP_TYPE_BOOK_ROW);
  G_OBJECT_CLASS (stamp_book_row_parent_class)->dispose (object);
}

static void
stamp_book_row_class_init (StampBookRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = stamp_book_row_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/contact/book-list/stamp-book-row.ui");

  gtk_widget_class_bind_template_child (widget_class, StampBookRow, image);
  gtk_widget_class_bind_template_child (widget_class, StampBookRow, inscription);
  gtk_widget_class_bind_template_child (widget_class, StampBookRow, spinner);
}

GtkWidget *
stamp_book_row_new (void)
{
  return g_object_new (STAMP_TYPE_BOOK_ROW, NULL);
}

void
stamp_book_row_unbind (StampBookRow *self)
{
  if (self->loading_binding) {
    g_binding_unbind (self->loading_binding);
    g_clear_object (&self->loading_binding);
  }
}

void
stamp_book_row_bind (StampBookRow *self,
                     StampItem    *item)
{
  const gchar *icon_name = stamp_item_get_icon_name (item);
  const gchar *name = stamp_item_get_name (item);

  stamp_book_row_unbind (self);
  /* Image */
  gtk_image_set_from_icon_name (GTK_IMAGE (self->image), icon_name);
  gtk_widget_set_visible (self->image, icon_name != NULL);

  gtk_widget_set_tooltip_text (GTK_WIDGET (self), name);

  if (STAMP_IS_BOOK_ACCOUNT_ITEM (item)) {
    gtk_image_set_pixel_size (GTK_IMAGE (self->image), 24);

    gtk_widget_add_css_class (GTK_WIDGET (self), "bold");
    gtk_inscription_set_text (GTK_INSCRIPTION (self->inscription), name);
  } else {
    gtk_image_set_pixel_size (GTK_IMAGE (self->image), -1);

    gtk_widget_remove_css_class (GTK_WIDGET (self), "bold");
    gtk_inscription_set_text (GTK_INSCRIPTION (self->inscription), name);
  }

  self->loading_binding = g_object_ref (g_object_bind_property (item, "loading", self->spinner, "visible", G_BINDING_SYNC_CREATE));
}
