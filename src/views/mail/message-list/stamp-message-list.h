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
#include "stamp-composer.h"

G_BEGIN_DECLS

#define STAMP_TYPE_MESSAGE_LIST (stamp_message_list_get_type())

G_DECLARE_FINAL_TYPE (StampMessageList, stamp_message_list, STAMP, MESSAGE_LIST, AdwBreakpointBin);

GtkWidget *
stamp_message_list_new (void);

void
stamp_message_list_set_conversation (StampMessageList      *self,
                                     StampAccount          *account,
                                     CamelFolderThreadNode *node);

gboolean
stamp_message_list_has_conversation (StampMessageList *self);

void
stamp_message_list_hovering_over_link (StampMessageList *self,
                                       const gchar       *title,
                                       const gchar       *url);

void
stamp_message_list_compose (StampMessageList  *self,
                            StampComposerType  type,
                            GVariant          *parameter);

void
stamp_message_list_print (StampMessageList *self,
                          GVariant         *parameter);

void
stamp_message_list_view_source (StampMessageList *self,
                                GVariant         *parameter);

void
stamp_message_list_set_external (StampMessageList *self,
                                 gboolean          is_external);

void
stamp_message_list_set_mobile_mode (StampMessageList *self,
                                    gboolean          mobile);

void
stamp_message_list_set_key_capture_widget (StampMessageList *self,
                                           gboolean          enable);

G_END_DECLS
