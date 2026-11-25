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

#include "config.h"

#include "stamp-settings.h"

#include <gio/gio.h>
#include <glib.h>

static GHashTable *settings = NULL;

GSettings *
stamp_settings_get (const gchar *schema)
{
  GSettings *gsettings = NULL;

  if (settings) {
    gsettings = g_hash_table_lookup (settings, schema);
    if (gsettings)
      return gsettings;
  }

  gsettings = g_settings_new (schema);
  if (!gsettings)
    g_warning ("%s: Invalid schema %s requested", G_STRFUNC, schema);
  else
    g_hash_table_insert (settings, g_strdup (schema), gsettings);

  return gsettings;
}

void
stamp_settings_init (void)
{
  settings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}

void
stamp_settings_shutdown (void)
{
  if (settings) {
    g_hash_table_remove_all (settings);
    g_clear_pointer (&settings, g_hash_table_unref);
  }
}
