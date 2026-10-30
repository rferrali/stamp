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

#pragma once

#include <adwaita.h>

#include "stamp-webview.h"

G_BEGIN_DECLS

#define STAMP_TYPE_RICH_TEXT_EDITOR (stamp_rich_text_editor_get_type ())
G_DECLARE_FINAL_TYPE (StampRichTextEditor, stamp_rich_text_editor, STAMP, RICH_TEXT_EDITOR, AdwBin)

GtkWidget *
stamp_rich_text_editor_new (void);

StampWebView *
stamp_rich_text_editor_get_web_view (StampRichTextEditor *self);

AdwWrapBox *
stamp_rich_text_editor_get_toolbar (StampRichTextEditor *self);

void
stamp_rich_text_editor_append_toolbar (StampRichTextEditor *self,
                                       GtkWidget           *child);

void
stamp_rich_text_editor_set_html (StampRichTextEditor *self,
                                 const gchar         *html);

void
stamp_rich_text_editor_set_plain_text (StampRichTextEditor *self,
                                       const gchar         *text);

void
stamp_rich_text_editor_get_html_async (StampRichTextEditor *self,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data);

gchar *
stamp_rich_text_editor_get_html_finish (StampRichTextEditor  *self,
                                        GAsyncResult         *result,
                                        gchar               **out_plain_text,
                                        GError              **error);

G_END_DECLS
