/*
 * Copyright 2024-2026 Jan-Michael Brummer
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

#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

#include "stamp-account.h"

G_BEGIN_DECLS

#define STAMP_TYPE_ACCOUNT_MANAGER (stamp_account_manager_get_type ())
G_DECLARE_FINAL_TYPE (StampAccountManager, stamp_account_manager, STAMP, ACCOUNT_MANAGER, GObject);

StampAccountManager *
stamp_account_manager_new (void);

void
stamp_account_manager_load_async (StampAccountManager *self,
                                  ESourceRegistry     *registry,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);

gboolean
stamp_account_manager_load_finish (StampAccountManager  *self,
                                   GAsyncResult         *result,
                                   GError              **error);

GList *
stamp_account_manager_get_accounts (StampAccountManager *self);

StampAccount *
stamp_account_manager_find_by_mail_uid (StampAccountManager *self,
                                        const gchar         *mail_uid);

gboolean
stamp_account_manager_remove_by_service (StampAccountManager *self,
                                         CamelService        *service);

G_END_DECLS
