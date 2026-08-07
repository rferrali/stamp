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

#include <camel/camel.h>
#include <glib.h>

#include "stamp-account.h"

G_BEGIN_DECLS

gboolean
stamp_scheduler_init (void);

void
stamp_scheduler_shutdown (void);

guint
stamp_scheduler_get_pending_count (void);

gboolean
stamp_scheduler_schedule (StampAccount         *account,
                          CamelMimeMessage     *message,
                          const gchar          *sender_name,
                          const gchar          *sender_mail,
                          CamelInternetAddress *all_recipients,
                          gboolean              pgp_sign,
                          gboolean              pgp_encrypt,
                          gboolean              smime_sign,
                          gboolean              smime_encrypt,
                          const gchar          *draft_uid,
                          gint64                send_at);

G_END_DECLS
