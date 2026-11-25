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

#include <libedataserver/libedataserver.h>

#include "stamp-signature.h"

G_BEGIN_DECLS

#define STAMP_TYPE_SIGNATURE_STORE (stamp_signature_store_get_type ())
G_DECLARE_FINAL_TYPE (StampSignatureStore, stamp_signature_store, STAMP, SIGNATURE_STORE, GObject);

StampSignatureStore *
stamp_signature_store_new (void);

void
stamp_signature_store_load (StampSignatureStore *self,
                            ESourceRegistry     *registry);

GList *
stamp_signature_store_get_signatures (StampSignatureStore *self);

StampSignature *
stamp_signature_store_create (StampSignatureStore *self,
                              const gchar         *name,
                              const gchar         *content,
                              const gchar         *mime_type);

void
stamp_signature_store_remove (StampSignatureStore *self,
                              StampSignature      *signature);

G_END_DECLS
