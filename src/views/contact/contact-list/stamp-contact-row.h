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

#include "stamp-account.h"
#include "stamp-contact-item.h"
#include "stamp-contact-list.h"

#pragma once

G_BEGIN_DECLS

#define STAMP_TYPE_CONTACT_ROW (stamp_contact_row_get_type ())

G_DECLARE_FINAL_TYPE (StampContactRow, stamp_contact_row, STAMP, CONTACT_ROW, AdwBin);

GtkWidget *
stamp_contact_row_new (void);

void
stamp_contact_row_bind_mail (StampContactRow  *self,
                             StampContactItem *item,
                             StampContactList *list);

void
stamp_contact_row_unbind (StampContactRow *self);

G_END_DECLS

