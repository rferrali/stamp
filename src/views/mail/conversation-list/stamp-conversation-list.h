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

#include <adwaita.h>
#include <camel/camel.h>

#include "stamp-account.h"
#include "stamp-collection-item.h"
#include "stamp-conversation-item.h"

G_BEGIN_DECLS

#define STAMP_TYPE_CONVERSATION_LIST (stamp_conversation_list_get_type())

G_DECLARE_FINAL_TYPE (StampConversationList, stamp_conversation_list, STAMP, CONVERSATION_LIST, AdwBin);

GtkWidget *
stamp_conversation_list_new (void);

void
stamp_conversation_list_load_folder (StampConversationList *self,
                                     StampAccount          *account,
                                     gchar                  *full_name);

void
stamp_conversation_list_refresh (StampConversationList *self);

void
stamp_conversation_list_load_collection (StampConversationList  *self,
                                         StampCollectionItem    *collection);

StampAccount *
stamp_conversation_list_get_selected_account (StampConversationList *self);

StampAccount *
stamp_conversation_list_get_account (StampConversationList *self);

gboolean
stamp_conversation_list_is_collection (StampConversationList *self);

void
stamp_conversation_list_clear (StampConversationList *self);

void
stamp_conversation_list_mark_read (StampConversationList *self,
                                   CamelFolderThreadNode *node);

void
stamp_conversation_list_mark_unread (StampConversationList *self,
                                     CamelFolderThreadNode *node);

void
stamp_conversation_list_mark_unflag_selected_messages (StampConversationList *self);

void
stamp_conversation_list_mark_flag_selected_messages (StampConversationList *self);

void
stamp_conversation_list_unselect (StampConversationList *self);

gboolean
stamp_conversation_list_trash (StampConversationList *self,
                                StampConversationItem *item);

void
stamp_conversation_list_undo_trash (StampConversationList *self);

void
stamp_conversation_list_finalize_trash (StampConversationList *self,
                                        gboolean               select_next);

void
stamp_mail_conversation_list_search_contact (StampConversationList *self,
                                             const gchar            *mail);

GtkToggleButton *
stamp_conversation_list_get_sidebar_button (StampConversationList *self);

void
stamp_consersation_list_set_show_buttons (StampConversationList *self,
                                          gboolean               show);

void
stamp_conversation_list_set_key_capture_widget (StampConversationList *self,
                                                gboolean               enable);

void
stamp_conversation_list_select_relative (StampConversationList *self,
                                         gint                    direction);

StampConversationItem *
stamp_conversation_list_get_adjacent_item (StampConversationList *self,
                                           gint                    offset);

void
stamp_conversation_list_junk (StampConversationList *self,
                               StampConversationItem *item);

void
stamp_conversation_list_archive (StampConversationList *self,
                                  StampConversationItem *item);

void
stamp_conversation_list_move_conversation (StampConversationList *self,
                                            StampConversationItem *item,
                                            const gchar           *target_full_name);

G_END_DECLS
