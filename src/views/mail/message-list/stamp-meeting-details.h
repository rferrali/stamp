/*
 * Copyright 2026 Jan-Michael Brummer
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
#include <libecal/libecal.h>
#include "stamp-account.h"

G_BEGIN_DECLS

#define STAMP_TYPE_MEETING_DETAILS (stamp_meeting_details_get_type ())
G_DECLARE_FINAL_TYPE (StampMeetingDetails, stamp_meeting_details, STAMP, MEETING_DETAILS, GtkBox);

GtkWidget *
stamp_meeting_details_new (StampAccount          *account,
                           ICalComponent         *calendar,
                           const gchar           *method,
                           const gchar           *email,
                           ICalParameterPartstat  saved_response,
                           gboolean               response_pending,
                           gboolean              *can_respond);

ICalParameterPartstat
stamp_meeting_details_get_saved_response (ICalComponent *invitation,
                                          ICalComponent *stored,
                                          const gchar   *email,
                                          gboolean       uid_resolved);

gboolean
stamp_meeting_details_is_cancelled (ICalComponent *calendar,
                                    const gchar   *method);

void
stamp_meeting_details_set_mobile_mode (GtkWidget *details,
                                       gboolean   mobile);

G_END_DECLS
