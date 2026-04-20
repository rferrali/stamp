/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define STAMP_TYPE_CALENDAR_LIST (stamp_calendar_list_get_type ())
G_DECLARE_FINAL_TYPE (StampCalendarList, stamp_calendar_list, STAMP, CALENDAR_LIST, AdwBin)

void stamp_calendar_list_set_model (StampCalendarList *self,
                                    GListModel        *model);

G_END_DECLS
