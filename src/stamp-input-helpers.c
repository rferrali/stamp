/*
 * Copyright 2024-2026 Jan-Michael Brummer
 *
 * This file is part of Stamp.
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

#include "stamp-input-helpers.h"

gboolean
stamp_input_has_keyboard (GtkWidget *widget)
{
  GdkSeat *seat = gdk_display_get_default_seat (gtk_widget_get_display (widget));

  return seat && (gdk_seat_get_capabilities (seat) & GDK_SEAT_CAPABILITY_KEYBOARD) != 0;
}
