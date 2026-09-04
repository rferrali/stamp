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

#include "stamp-collection-item.h"
#include "stamp-conversation-list.h"

G_BEGIN_DECLS

#define STAMP_TYPE_FOLDER_LIST (stamp_folder_list_get_type())

G_DECLARE_FINAL_TYPE (StampFolderList, stamp_folder_list, STAMP, FOLDER_LIST, AdwBin);

GtkWidget *
stamp_folder_list_new (void);

void
stamp_folder_list_unselect (StampFolderList *self);

void
stamp_folder_list_set_conversation_list (StampFolderList        *self,
                                          StampConversationList *conversation_list);

G_END_DECLS
