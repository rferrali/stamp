/*
 * Copyright 2025 Jan-Michael Brummer
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

#include <gtk/gtk.h>

#include "stamp-item.h"

G_BEGIN_DECLS

#define STAMP_TYPE_BOOK_ROW (stamp_book_row_get_type ())
G_DECLARE_FINAL_TYPE (StampBookRow, stamp_book_row, STAMP, BOOK_ROW, GtkBox);

GtkWidget *
stamp_book_row_new (void);

void
stamp_book_row_bind (StampBookRow *self,
                     StampItem    *item);

void
stamp_book_row_unbind (StampBookRow *self);

G_END_DECLS

