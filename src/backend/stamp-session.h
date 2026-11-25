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

#include "stamp-account-manager.h"
#include "stamp-signature-store.h"

G_BEGIN_DECLS

#define STAMP_TYPE_SESSION (stamp_session_get_type ())
G_DECLARE_FINAL_TYPE (StampSession, stamp_session, STAMP, SESSION, CamelSession);

typedef gboolean (*StampSessionCredentialsFunc) (StampSession  *session,
                                                 ESource       *source,
                                                 CamelService  *service,
                                                 const gchar   *mechanism,
                                                 GCancellable  *cancellable,
                                                 GError       **error,
                                                 gpointer       user_data);

StampSession *
stamp_session_get_default (void);

void
stamp_session_set_credentials_func (StampSession                *self,
                                    StampSessionCredentialsFunc  func,
                                    gpointer                     user_data);

StampAccountManager *
stamp_session_get_account_manager (StampSession *self);

StampSignatureStore *
stamp_session_get_signature_store (StampSession *self);

ESourceRegistry *
stamp_session_get_registry (StampSession *self);

G_END_DECLS

