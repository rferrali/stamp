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

#include <adwaita.h>

#include <camel/camel.h>

#include "stamp-account.h"
#include "stamp-webview.h"

#pragma once

G_BEGIN_DECLS

#define STAMP_TYPE_MESSAGE_LIST_ITEM (stamp_message_list_item_get_type ())

G_DECLARE_FINAL_TYPE (StampMessageListItem, stamp_message_list_item, STAMP, MESSAGE_LIST_ITEM, GtkListBoxRow);

GtkWidget *
stamp_message_list_item_new (StampAccount          *account,
                             CamelFolderThreadNode *thread_node);

void
stamp_message_list_item_set_expanded (StampMessageListItem *self,
                                      gboolean              expanded);

const CamelMessageInfo *
stamp_message_list_item_get_message_info (StampMessageListItem *self);

void stamp_message_list_item_get_message_body_html (StampMessageListItem *self,
                                                    GCancellable         *cancellable,
                                                    GAsyncReadyCallback   callback,
                                                    gpointer              user_data);

gchar *stamp_message_list_item_get_message_body_html_finish (StampMessageListItem  *self,
                                                            GAsyncResult          *res,
                                                            GError               **error);

CamelMimeMessage *
stamp_message_list_item_get_message (StampMessageListItem *self);

guint64
stamp_message_list_item_get_timestamp (StampMessageListItem *self);

void
stamp_message_list_item_print (StampMessageListItem *self);

const gchar *
stamp_message_list_item_get_uid (StampMessageListItem *self);

void
stamp_message_list_item_view_source (StampMessageListItem *self);

typedef void (*StampMessageListItemSearchDone) (StampMessageListItem *item,
                                                gboolean               found,
                                                gpointer               user_data);

void
stamp_message_list_item_search (StampMessageListItem           *self,
                                const gchar                    *search_text,
                                StampMessageListItemSearchDone  done,
                                gpointer                        user_data);

StampWebView *
stamp_message_list_item_get_web_view (StampMessageListItem *self);

G_END_DECLS


