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

#include "stamp-window.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libportal-gtk4/portal-gtk4.h>

#include "stamp-account.h"
#include "stamp-application.h"
#include "stamp-calendar-view.h"
#include "stamp-contact-view.h"
#include "stamp-helper.h"
#include "stamp-mail-view.h"
#include "stamp-session.h"
#include "stamp-settings.h"

struct _StampWindow {
  AdwApplicationWindow parent_instance;

  AdwViewStack *app_view_stack;
  AdwViewStack *main_view_stack;
  StampMailView *mail_view;
  StampContactView *contact_view;

  gint current_width;
  gint current_height;
  guint has_default_size : 1;
  guint is_maximized : 1;
};

G_DEFINE_FINAL_TYPE (StampWindow, stamp_window, ADW_TYPE_APPLICATION_WINDOW);

static void
on_open_settings_clicked (GtkWidget     *listbox,
                          GtkListBoxRow *row,
                          StampWindow   *self)
{
  stamp_launch_goa ();
}

static void
on_account_changed (GObject      *object,
                    StampAccount *account,
                    gpointer      user_data)
{
  StampWindow *self = STAMP_WINDOW (user_data);
  StampSession *session = STAMP_SESSION (object);
  GList *accounts = stamp_session_get_accounts (session);

  if (!accounts) {
    adw_view_stack_set_visible_child_name (self->app_view_stack, "welcome");
  } else {
    adw_view_stack_set_visible_child_name (self->app_view_stack, "main");
  }
}

static void
on_show_mail (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data)
{
  StampWindow *self = STAMP_WINDOW (user_data);

  adw_view_stack_set_visible_child_name (self->main_view_stack, "mail");
}

static void
on_show_contacts (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  StampWindow *self = STAMP_WINDOW (user_data);

  adw_view_stack_set_visible_child_name (self->main_view_stack, "contacts");
}

static void
on_show_calendar (GSimpleAction *action G_GNUC_UNUSED,
                  GVariant *parameter   G_GNUC_UNUSED,
                  gpointer              user_data)
{
  StampWindow *self = STAMP_WINDOW (user_data);

  adw_view_stack_set_visible_child_name (self->main_view_stack, "calendar");
}

static gboolean
on_close_request (GtkWindow *source,
                  gpointer   user_data)
{
  if (gtk_window_get_hide_on_close (source))
    return FALSE;

  return !stamp_application_confirm_quit ();
}

static const GActionEntry stamp_window_action_entries[] = {
  { .name = "show-mail", .activate = on_show_mail },
  { .name = "show-contacts", .activate = on_show_contacts },
  { .name = "show-calendar", .activate = on_show_calendar },
};

static void
stamp_window_dispose (GObject *object)
{
  StampWindow *self = STAMP_WINDOW (object);
  StampSession *session = stamp_session_get_default ();
  const gchar *view;

  view = adw_view_stack_get_visible_child_name (self->main_view_stack);
  if (view)
    g_settings_set (STAMP_SETTINGS, STAMP_PREFS_VIEW, "s", view);

  g_signal_handlers_disconnect_by_data (session, self);

  if (self->mail_view)
    g_signal_handlers_disconnect_by_data (self->mail_view, self);

  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_WINDOW);

  G_OBJECT_CLASS (stamp_window_parent_class)->dispose (object);
}

static void
stamp_window_class_init (StampWindowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = stamp_window_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/stamp-window.ui");

  gtk_widget_class_bind_template_child (widget_class, StampWindow, app_view_stack);
  gtk_widget_class_bind_template_child (widget_class, StampWindow, main_view_stack);
  gtk_widget_class_bind_template_child (widget_class, StampWindow, mail_view);
  gtk_widget_class_bind_template_child (widget_class, StampWindow, contact_view);

  gtk_widget_class_bind_template_callback (widget_class, on_open_settings_clicked);
}

static void
on_background_status (GObject      *source_object,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  XdpPortal *portal = XDP_PORTAL (source_object);
  g_autoptr (GError) error = NULL;

  if (!xdp_portal_set_background_status_finish (portal, res, &error)) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("%s: Could not set background status: %s", G_STRFUNC, error->message);
    return;
  }
}

static gboolean
stamp_is_running_inside_flatpak (void)
{
  return g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS);
}

static void
stamp_window_init (StampWindow *self)
{
  StampSession *session = stamp_session_get_default ();
  g_autofree char *view = NULL;
  XdpPortal *portal = xdp_portal_new ();
  g_autoptr (XdpParent) parent_window = xdp_parent_new_gtk (GTK_WINDOW (self));

  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self, "close-request", G_CALLBACK (on_close_request), self);

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   stamp_window_action_entries,
                                   G_N_ELEMENTS (stamp_window_action_entries),
                                   self);

  g_signal_connect_object (session, "account-added", G_CALLBACK (on_account_changed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (session, "account-removed", G_CALLBACK (on_account_changed), self, G_CONNECT_DEFAULT);

  if (stamp_is_running_inside_flatpak () && g_settings_get_boolean (STAMP_SETTINGS, STAMP_PREFS_BACKGROUND_NOTIFICATIONS))
    xdp_portal_set_background_status (portal, _("Waiting for new emails"), NULL, on_background_status, self);

  g_settings_bind (STAMP_SETTINGS, STAMP_PREFS_BACKGROUND_NOTIFICATIONS, self, "hide-on-close", G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (STAMP_SETTINGS, STAMP_PREFS_WINDOW_WIDTH, self, "default-width", G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (STAMP_SETTINGS, STAMP_PREFS_WINDOW_HEIGHT, self, "default-height", G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (STAMP_SETTINGS, STAMP_PREFS_WINDOW_MAXIMIZED, self, "maximized", G_SETTINGS_BIND_DEFAULT);
  g_settings_get (STAMP_SETTINGS, STAMP_PREFS_VIEW, "s", &view);

  if (view)
    adw_view_stack_set_visible_child_name (self->main_view_stack, view);

#ifdef DEVEL
  gtk_widget_add_css_class (GTK_WIDGET (self), "devel");
#endif
}

void
stamp_window_search_contact (StampWindow *self,
                             const gchar *mail)
{
  stamp_mail_view_search_contact (self->mail_view, mail);
}

void
stamp_window_show_contact (StampWindow *self,
                           const gchar *mail)
{
  stamp_contact_view_show_contact (self->contact_view, mail);
  adw_view_stack_set_visible_child_name (self->main_view_stack, "contacts");
}

StampMailView *
stamp_window_get_mail_view (StampWindow *self)
{
  return self->mail_view;
}

StampWindow *
stamp_get_main_window (void)
{
  GtkApplication *app = GTK_APPLICATION (g_application_get_default ());
  GList *windows = gtk_application_get_windows (app);

  for (GList *win = windows; win && win->data; win = g_list_next (win)) {
    if (STAMP_IS_WINDOW (win->data))
      return STAMP_WINDOW (win->data);
  }

  return NULL;
}

void
stamp_window_show_mail_view (StampWindow *self)
{
  adw_view_stack_set_visible_child_name (self->main_view_stack, "mail");
}
