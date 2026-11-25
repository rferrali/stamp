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

#include "stamp-helper.h"

static gchar *cache_dir = NULL;
static gchar *data_dir = NULL;
static GMutex dir_mutex;
static gsize dir_mutex_initialized = 0;

static void
init_dir_mutex (void)
{
  if (g_once_init_enter (&dir_mutex_initialized)) {
    g_mutex_init (&dir_mutex);
    g_once_init_leave (&dir_mutex_initialized, 1);
  }
}

const gchar *
stamp_get_cache_dir (void)
{
  init_dir_mutex ();
  g_mutex_lock (&dir_mutex);
  if (!cache_dir)
    cache_dir = g_build_path (G_DIR_SEPARATOR_S, g_get_user_cache_dir (), "stamp", NULL);
  g_mutex_unlock (&dir_mutex);

  return cache_dir;
}

const gchar *
stamp_get_data_dir (void)
{
  init_dir_mutex ();
  g_mutex_lock (&dir_mutex);
  if (!data_dir)
    data_dir = g_build_path (G_DIR_SEPARATOR_S, g_get_user_data_dir (), "stamp", NULL);
  g_mutex_unlock (&dir_mutex);

  return data_dir;
}

gchar *
stamp_strip_department (const gchar *str)
{
  gchar *ret = g_strdup (str);
  const gchar *pos;

  pos = strchr (ret, '(');
  if (pos)
    ret[pos - ret] = '\0';

  return ret;
}

gchar **
g_strv_remove (const gchar * const *strv,
               const gchar         *str)
{
  gchar **new_strv;
  gchar **n;
  const gchar * const *s;
  guint len;

  if (!g_strv_contains (strv, str))
    return g_strdupv ((char **)strv);

  /* Needs room for one fewer string than before, plus one for trailing NULL. */
  len = g_strv_length ((char **)strv);
  new_strv = g_malloc ((len - 1 + 1) * sizeof (char *));
  n = new_strv;
  s = strv;

  while (*s != NULL) {
    if (!g_str_equal (*s, str)) {
      *n = g_strdup (*s);
      n++;
    }
    s++;
  }
  new_strv[len - 1] = NULL;

  return new_strv;
}

gchar **
g_strv_append (const gchar * const *strv,
               const gchar         *str)
{
  gchar **new_strv;
  gchar **n;
  const gchar * const *s;
  guint len;

  if (g_strv_contains (strv, str))
    return g_strdupv ((char **)strv);

  /* Needs room for one fewer string than before, plus one for trailing NULL. */
  len = g_strv_length ((char **)strv) + 2;
  new_strv = g_malloc (len * sizeof (char *));
  n = new_strv;
  s = strv;

  while (*s != NULL) {
    *n = g_strdup (*s);
    n++;
    s++;
  }

  new_strv[len - 2] = g_strdup (str);
  new_strv[len - 1] = NULL;

  return new_strv;
}
