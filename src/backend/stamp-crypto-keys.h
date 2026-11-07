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

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  GPtrArray *pgp_labels;
  GPtrArray *pgp_key_ids;
  GPtrArray *smime_labels;
  GPtrArray *smime_cert_nicknames;
} StampCryptoKeys;

StampCryptoKeys *
stamp_crypto_keys_ref (StampCryptoKeys *keys);

void
stamp_crypto_keys_unref (StampCryptoKeys *keys);

void
stamp_crypto_keys_load_async (GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

StampCryptoKeys *
stamp_crypto_keys_load_finish (GAsyncResult  *result,
                               GError       **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (StampCryptoKeys, stamp_crypto_keys_unref)

G_END_DECLS
