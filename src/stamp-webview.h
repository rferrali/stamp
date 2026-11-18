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

#include <gtk/gtk.h>

#include <webkit/webkit.h>

#pragma once

G_BEGIN_DECLS

#define STAMP_TYPE_WEBVIEW (stamp_webview_get_type ())

G_DECLARE_FINAL_TYPE (StampWebView, stamp_webview, STAMP, WEB_VIEW, WebKitWebView);

typedef GInputStream *(*cid_handler_func)(WebKitURISchemeRequest *request,
                                          gpointer                user_data);

StampWebView *
stamp_webview_new (void);

void
stamp_webview_load_html (StampWebView *self,
                         gchar         *content);

void
stamp_webview_add_internal_resource (StampWebView *self,
                                     const gchar   *id,
                                     GInputStream *stream);

void
stamp_webview_load_plain_text (StampWebView *self,
                               gchar         *content);

void
stamp_webview_get_body_html (StampWebView        *self,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data);

gchar *
stamp_webview_get_body_html_finish_full (StampWebView  *self,
                                         GAsyncResult  *res,
                                         gchar        **out_plain_text,
                                         gchar        **out_unquoted_text,
                                         GError       **error);

gchar *
stamp_webview_get_body_html_finish (StampWebView  *self,
                                    GAsyncResult  *res,
                                    gchar         **out_plain_text,
                                    GError       **error);

void
stamp_web_view_set_body_content (StampWebView *self,
                                 gchar         *content);

void
stamp_web_view_load_images (StampWebView *self);

void
stamp_web_view_query_command_state (StampWebView        *self,
                                    const gchar          *command,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);

gboolean
stamp_web_view_query_command_state_finish (GObject       *source,
                                           GAsyncResult  *res,
                                           GError       **error);

void
stamp_web_view_execute_editor_command (StampWebView *self,
                                       const gchar   *command,
                                       const gchar   *argument);

void
stamp_web_view_get_size (StampWebView *self,
                         gint32       *width,
                         gint32       *height);

gboolean
stamp_web_view_changed (StampWebView *self);

void
stamp_webview_set_editable (StampWebView *self);

void
stamp_webview_copy_resources (StampWebView *src,
                              StampWebView *dst);

void
stamp_webview_set_cid_handler (StampWebView     *self,
                               cid_handler_func  cid_handler,
                               gpointer          user_data);

G_END_DECLS

