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
#include "stamp-conversation-item.h"

#pragma once

G_BEGIN_DECLS

#define STAMP_TYPE_CONVERSATION_ROW (stamp_conversation_row_get_type ())

G_DECLARE_FINAL_TYPE (StampConversationRow, stamp_conversation_row, STAMP, CONVERSATION_ROW, GtkBox);

GtkWidget *
stamp_conversation_row_new (void);

void
stamp_conversation_row_bind_mail (StampConversationRow  *self,
                                  StampConversationItem *item,
                                  StampAccount          *account,
                                  gboolean               show_account);

void
stamp_conversation_row_unbind_mail (StampConversationRow  *self,
                                    StampConversationItem *item);

void
stamp_conversation_row_set_selection_visible (StampConversationRow *self,
                                              gboolean              visible);

void
stamp_conversation_row_set_selection_active (StampConversationRow *self,
                                             gboolean              active);

gboolean
stamp_conversation_row_get_selection_active (StampConversationRow *self);

StampConversationItem *
stamp_conversation_row_get_item (StampConversationRow *self);

GtkCheckButton *
stamp_conversation_row_get_check_button (StampConversationRow *self);

G_END_DECLS

