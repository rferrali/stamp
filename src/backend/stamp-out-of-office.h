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

#include "stamp-account.h"

G_BEGIN_DECLS

typedef enum {
  STAMP_OUT_OF_OFFICE_STATE_DISABLED,
  STAMP_OUT_OF_OFFICE_STATE_ENABLED,
  STAMP_OUT_OF_OFFICE_STATE_SCHEDULED,
} StampOutOfOfficeState;

typedef struct {
  guint state;
  guint audience;
  GDateTime *start;
  GDateTime *end;
  gchar *internal_reply;
  gchar *external_reply;
} StampOutOfOffice;

gboolean
stamp_out_of_office_supported (StampAccount *account);

void
stamp_out_of_office_free (StampOutOfOffice *settings);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (StampOutOfOffice, stamp_out_of_office_free)

void
stamp_out_of_office_load (StampAccount        *account,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data);

StampOutOfOffice *
stamp_out_of_office_load_finish (StampAccount  *account,
                                 GAsyncResult  *result,
                                 GError       **error);

void
stamp_out_of_office_save (StampAccount        *account,
                          StampOutOfOffice    *settings,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data);

gboolean
stamp_out_of_office_save_finish (StampAccount  *account,
                                 GAsyncResult  *result,
                                 GError       **error);

G_END_DECLS
