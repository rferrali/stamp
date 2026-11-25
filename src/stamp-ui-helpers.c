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

#include "stamp-ui-helpers.h"

#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>

#include "stamp-application.h"

void
stamp_launch_goa (void)
{
  const gchar *flatpak_id = getenv ("FLATPAK_ID");

  if (flatpak_id) {
    g_spawn_command_line_async ("flatpak-spawn --host gnome-control-center online-accounts", NULL);
  } else {
    StampApplication *self = STAMP_APPLICATION (g_application_get_default ());
    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (self));
    g_autoptr (GError) error = NULL;
    g_autoptr (GDesktopAppInfo) app_info = g_desktop_app_info_new ("gnome-online-accounts-panel.desktop");
    g_autoptr (GdkAppLaunchContext) context = NULL;

    context = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (window)));
    g_app_info_launch (G_APP_INFO (app_info), NULL, G_APP_LAUNCH_CONTEXT (context), &error);
  }
}
