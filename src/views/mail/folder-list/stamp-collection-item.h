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

#include <camel/camel.h>
#include <gtk/gtk.h>

#include "stamp-account.h"
#include "stamp-item.h"

#pragma once

G_BEGIN_DECLS

#define STAMP_TYPE_COLLECTION_ITEM (stamp_collection_item_get_type ())

typedef enum {
  STAMP_COLLECTION_COMBINED_INBOX,
} StampCollectionType;

G_DECLARE_FINAL_TYPE (StampCollectionItem, stamp_collection_item, STAMP, COLLECTION_ITEM, StampItem);

typedef struct {
  StampAccount *account;
  gchar *full_name;
  CamelFolder *folder;
  gint unread;
  gboolean loaded;
} StampCollectionSource;

StampCollectionItem *
stamp_collection_item_new (StampCollectionType type);

StampCollectionType
stamp_collection_item_get_collection_type (StampCollectionItem *self);

void
stamp_collection_item_add_source (StampCollectionItem *self,
                                  StampAccount         *account,
                                  const gchar          *full_name);

void
stamp_collection_item_remove_source (StampCollectionItem *self,
                                     StampAccount         *account);

void
stamp_collection_item_clear (StampCollectionItem *self);

GList *
stamp_collection_item_get_sources (StampCollectionItem *self);

void
stamp_collection_item_load (StampCollectionItem *self);

G_END_DECLS
