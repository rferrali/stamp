/*
 * Copyright 2025 Jan-Michael Brummer
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

#include <adwaita.h>
#include <libebook/libebook.h>

G_BEGIN_DECLS

#define STAMP_TYPE_CONTACT_ITEM (stamp_contact_item_get_type())
G_DECLARE_FINAL_TYPE (StampContactItem, stamp_contact_item, STAMP, CONTACT_ITEM, GObject);

StampContactItem *
stamp_contact_item_new (EContact *c);

const gchar *
stamp_contact_item_get_name (StampContactItem *self);

const gchar *
stamp_contact_item_get_given_name (StampContactItem *self);

const gchar *
stamp_contact_item_get_family_name (StampContactItem *self);

const gchar *
stamp_contact_item_get_mail (StampContactItem *self);

const gchar *
stamp_contact_item_get_org (StampContactItem *self);

const gchar *
stamp_contact_item_get_office (StampContactItem *self);

EContact *
stamp_contact_item_get_contact (StampContactItem *self);

GdkTexture *
stamp_contact_item_get_photo (StampContactItem *self);

G_END_DECLS
