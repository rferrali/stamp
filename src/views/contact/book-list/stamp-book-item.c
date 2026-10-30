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

#include "stamp-book-item.h"

#include "stamp-item.h"

struct _StampBookItem {
  StampItem parent_instance;

  EClient *client;
};

G_DEFINE_FINAL_TYPE (StampBookItem, stamp_book_item, STAMP_TYPE_ITEM);

typedef enum {
  PROP_CLIENT = 1,
} StampBookItemProps;

static GParamSpec *obj_properties[PROP_CLIENT + 1];

static void
stamp_book_item_set_property (GObject      *object,
                              guint         property_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  StampBookItem *self = STAMP_BOOK_ITEM (object);

  switch ((StampBookItemProps)property_id) {
    case PROP_CLIENT:
      g_clear_object (&self->client);

      self->client = g_value_get_object (value);
      if (self->client)
        g_object_ref (self->client);
      break;
  }
}

static void
stamp_book_item_get_property (GObject    *object,
                              guint       property_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  switch ((StampBookItemProps)property_id) {
    case PROP_CLIENT:
      break;
  }
}

static void
stamp_book_item_constructed (GObject *object)
{
  StampBookItem *self = STAMP_BOOK_ITEM (object);
  ESource *source;

  G_OBJECT_CLASS (stamp_book_item_parent_class)->constructed (object);

  source = e_client_get_source (self->client);

  stamp_item_set_name (STAMP_ITEM (self), e_source_get_display_name (source));
  stamp_item_set_icon_name (STAMP_ITEM (self), "x-office-address-book-symbolic");
}

static void
stamp_book_item_dispose (GObject *object)
{
  g_clear_object (&STAMP_BOOK_ITEM (object)->client);
  G_OBJECT_CLASS (stamp_book_item_parent_class)->dispose (object);
}

void
stamp_book_item_class_init (StampBookItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = stamp_book_item_constructed;
  object_class->dispose = stamp_book_item_dispose;
  object_class->set_property = stamp_book_item_set_property;
  object_class->get_property = stamp_book_item_get_property;

  obj_properties[PROP_CLIENT] =
    g_param_spec_object ("client",
                         NULL, NULL,
                         E_TYPE_CLIENT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (obj_properties), obj_properties);
}

void
stamp_book_item_init (StampBookItem *self)
{
}

StampBookItem *
stamp_book_item_new (StampAccount *account,
                     EClient      *client)
{
  return g_object_new (STAMP_TYPE_BOOK_ITEM,
                       "account", account,
                       "client", client,
                       NULL);
}

EClient *
stamp_book_item_get_client (StampBookItem *self)
{
  return self->client;
}
