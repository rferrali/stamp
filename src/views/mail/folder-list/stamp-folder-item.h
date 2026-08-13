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

#include <gtk/gtk.h>

#include "stamp-account.h"
#include "stamp-item.h"

#pragma once

G_BEGIN_DECLS

#define STAMP_TYPE_FOLDER_ITEM (stamp_folder_item_get_type ())

G_DECLARE_FINAL_TYPE (StampFolderItem, stamp_folder_item, STAMP, FOLDER_ITEM, StampItem);

StampFolderItem *
stamp_folder_item_new (StampAccount    *account,
                       CamelFolderInfo *folder_info);

CamelFolder *
stamp_folder_item_get_folder (StampFolderItem *self);

const gchar *
stamp_folder_item_get_full_name (StampFolderItem *self);

void
stamp_folder_item_set_folder_info (StampFolderItem *self,
                                   CamelFolderInfo *info);

gint
stamp_folder_item_get_flags (StampFolderItem *self);

void
stamp_folder_item_disconnect (StampFolderItem *self);

gint
stamp_folder_item_get_order_rank (StampFolderItem *self);

G_END_DECLS

