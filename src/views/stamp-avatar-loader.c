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

#include "stamp-avatar-loader.h"

typedef struct {
  GFunc callback;
  gpointer user_data;
} LookupData;

static GHashTable *textures;

static void
on_texture_finalized (gpointer  data,
                      GObject  *where_the_object_was)
{
  GBytes *bytes = data;

  if (textures)
    g_hash_table_remove (textures, bytes);

  g_bytes_unref (bytes);
}

static GdkTexture *
get_texture (GBytes *bytes)
{
  GdkTexture *texture;

  if (!textures)
    textures = g_hash_table_new_full (g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref, NULL);

  texture = g_hash_table_lookup (textures, bytes);
  if (texture)
    return g_object_ref (texture);

  texture = gdk_texture_new_from_bytes (bytes, NULL);
  if (texture) {
    g_hash_table_insert (textures, g_bytes_ref (bytes), texture);
    g_object_weak_ref (G_OBJECT (texture), on_texture_finalized, g_bytes_ref (bytes));
  }

  return texture;
}

static void
on_photo (gpointer photo,
          gpointer user_data)
{
  g_autofree LookupData *data = user_data;
  g_autoptr (GBytes) bytes = photo;
  g_autoptr (GdkTexture) texture = bytes ? get_texture (bytes) : NULL;

  data->callback (texture, data->user_data);
}

void
stamp_avatar_loader_lookup (StampAccount *account,
                            const gchar  *sender,
                            GCancellable *cancellable,
                            GFunc         callback,
                            gpointer      user_data)
{
  LookupData *data = g_new0 (LookupData, 1);

  data->callback = callback;
  data->user_data = user_data;

  stamp_account_get_photo (account, sender, cancellable, on_photo, data);
}
