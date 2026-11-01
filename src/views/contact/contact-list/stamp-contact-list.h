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


G_BEGIN_DECLS

#define STAMP_TYPE_CONTACT_LIST (stamp_contact_list_get_type())

G_DECLARE_FINAL_TYPE (StampContactList, stamp_contact_list, STAMP, CONTACT_LIST, AdwBreakpointBin);

typedef enum {
  SORT_MODE_GIVEN_NAME,
  SORT_MODE_FAMILY_NAME,
} StampSortMode;

GtkWidget *
stamp_contact_list_new (void);

void
stamp_contact_list_load (StampContactList *self,
                         EClient          *client,
                         StampAccount     *account);

void
stamp_contact_list_unselect (StampContactList *self);

StampAccount *
stamp_contact_list_get_account (StampContactList *self);

EClient *
stamp_contact_list_get_client (StampContactList *self);

GtkWidget *
stamp_contact_list_get_sidebar_button (StampContactList *self);

void
stamp_contact_list_search_contact (StampContactList *self,
                                   const gchar       *mail);

void
stamp_contact_list_set_show_buttons (StampContactList *self,
                                     gboolean          show);

void
stamp_contact_list_set_key_capture_widget (StampContactList *self,
                                           gboolean          enable);

G_END_DECLS
