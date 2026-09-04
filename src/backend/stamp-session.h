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
#include <gtk/gtk.h>

#include "stamp-signature.h"

G_BEGIN_DECLS

typedef struct _StampAccount StampAccount;

#define STAMP_TYPE_SESSION (stamp_session_get_type ())
G_DECLARE_FINAL_TYPE (StampSession, stamp_session, STAMP, SESSION, CamelSession);

StampSession *
stamp_session_get_default (void);

GList *
stamp_session_get_accounts (StampSession *self);

StampAccount *
stamp_session_find_account_by_mail_uid (StampSession *self,
                                        const gchar   *mail_uid);

GList *
stamp_session_get_signatures (StampSession *self);

ESourceRegistry *
stamp_session_get_registry (StampSession *self);

void
stamp_session_remove_signature (StampSession   *self,
                                StampSignature *signature);

StampSignature *
stamp_session_create_signature (StampSession *self,
                                const gchar  *name,
                                const gchar  *content,
                                const gchar  *mime_type);

G_END_DECLS

