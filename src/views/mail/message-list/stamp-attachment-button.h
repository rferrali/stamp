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

#pragma once

#include <camel/camel.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define STAMP_TYPE_ATTACHMENT_BUTTON (stamp_attachment_button_get_type ())
G_DECLARE_FINAL_TYPE (StampAttachmentButton, stamp_attachment_button, STAMP, ATTACHMENT_BUTTON, GtkBox);

GtkWidget *
stamp_attachment_button_new (CamelMimePart *mime_part);

GtkWidget *
stamp_attachment_button_new_from_data (const gchar *filename,
                                       const gchar *content_type,
                                       gsize        size,
                                       GBytes      *data,
                                       gboolean     editable);

GtkWidget *
stamp_attachment_button_new_from_file (GFile *file);

void
stamp_attachment_button_activate (StampAttachmentButton *self);

CamelMimePart *
stamp_attachment_button_get_mime_part (StampAttachmentButton *self);

const gchar *
stamp_attachment_button_get_filename (StampAttachmentButton *self);

gboolean
stamp_attachment_button_save_to_file (StampAttachmentButton *self,
                                      GFile                *file,
                                      GError              **error);

G_END_DECLS
