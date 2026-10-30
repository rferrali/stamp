/*
 * Copyright 2025-2026 Jan-Michael Brummer
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

#include "stamp-contact-item.h"

#include <camel/camel.h>
#include <glib/gi18n.h>

struct _StampContactItem {
  GObject parent_instance;

  EContact *contact;
};

G_DEFINE_FINAL_TYPE (StampContactItem, stamp_contact_item, G_TYPE_OBJECT);

static void
stamp_contact_item_dispose (GObject *object)
{
  StampContactItem *self = STAMP_CONTACT_ITEM (object);

  g_clear_object (&self->contact);

  G_OBJECT_CLASS (stamp_contact_item_parent_class)->dispose (object);
}

static void
stamp_contact_item_constructed (GObject *object)
{
  /* StampContactItem *self = STAMP_CONTACT_ITEM (object); */

  G_OBJECT_CLASS (stamp_contact_item_parent_class)->constructed (object);
}

void
stamp_contact_item_class_init (StampContactItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = stamp_contact_item_dispose;
  object_class->constructed = stamp_contact_item_constructed;
}

void
stamp_contact_item_init (StampContactItem *self)
{
}

StampContactItem *
stamp_contact_item_new (EContact *contact)
{
  StampContactItem *ret;
  ret = g_object_new (STAMP_TYPE_CONTACT_ITEM,
                      NULL);

  ret->contact = g_object_ref (contact);
  return ret;
}

const gchar *
stamp_contact_item_get_name (StampContactItem *self)
{
  return e_contact_get_const (self->contact, E_CONTACT_FULL_NAME);
}

const gchar *
stamp_contact_item_get_given_name (StampContactItem *self)
{
  return e_contact_get_const (self->contact, E_CONTACT_GIVEN_NAME);
}

const gchar *
stamp_contact_item_get_family_name (StampContactItem *self)
{
  return e_contact_get_const (self->contact, E_CONTACT_FAMILY_NAME);
}

const gchar *
stamp_contact_item_get_mail (StampContactItem *self)
{
  return e_contact_get_const (self->contact, E_CONTACT_EMAIL_1);
}

const gchar *
stamp_contact_item_get_org (StampContactItem *self)
{
  return e_contact_get_const (self->contact, E_CONTACT_ORG);
}

const gchar *
stamp_contact_item_get_office (StampContactItem *self)
{
  return e_contact_get_const (self->contact, E_CONTACT_OFFICE);
}

EContact *
stamp_contact_item_get_contact (StampContactItem *self)
{
  return self->contact;
}

GdkTexture *
stamp_contact_item_get_photo (StampContactItem *self)
{
  EContact *contact = stamp_contact_item_get_contact (self);
  g_autoptr (EContactPhoto) photo = NULL;
  GdkTexture *texture = NULL;

  if (!contact)
    return NULL;

  photo = e_contact_get (contact, E_CONTACT_PHOTO);
  if (!photo)
    return NULL;

  if (photo->type == E_CONTACT_PHOTO_TYPE_INLINED) {
    g_autoptr (GBytes) bytes = g_bytes_new (photo->data.inlined.data, photo->data.inlined.length);

    texture = gdk_texture_new_from_bytes (bytes, NULL);
  } else if (photo->type == E_CONTACT_PHOTO_TYPE_URI && g_str_has_prefix (photo->data.uri, "file://")) {
    g_autoptr (GFile) file = g_file_new_for_uri (photo->data.uri);

    texture = gdk_texture_new_from_file (file, NULL);
  }

  return texture;
}
