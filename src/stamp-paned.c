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

#include <config.h>

#include "stamp-paned.h"

#include "stamp-settings.h"

#define MIN_POSITION 50

typedef struct {
  GtkPaned *paned;
  gchar *settings_key;
  gint position;
  gint last_width;
  guint save_handler;
  gboolean dragging;
} PanedState;

static void
save_position (PanedState *state)
{
  g_settings_set_int (STAMP_SETTINGS, state->settings_key, state->position);
}

static void
on_save_timeout (gpointer user_data)
{
  PanedState *state = user_data;

  state->save_handler = 0;
  save_position (state);
}

static void
paned_state_free (PanedState *state)
{
  if (state->save_handler) {
    g_source_remove (state->save_handler);
    save_position (state);
  }

  g_free (state->settings_key);
  g_free (state);
}

static gboolean
on_event (GtkEventControllerLegacy *controller G_GNUC_UNUSED,
          GdkEvent                            *event,
          gpointer                             user_data)
{
  PanedState *state = user_data;
  GdkEventType type = gdk_event_get_event_type (event);

  if (type == GDK_BUTTON_PRESS)
    state->dragging = TRUE;
  else if (type == GDK_BUTTON_RELEASE || type == GDK_GRAB_BROKEN)
    state->dragging = FALSE;

  return GDK_EVENT_PROPAGATE;
}

static void
on_position_changed (GtkPaned      *paned,
                     GParamSpec *ps G_GNUC_UNUSED,
                     gpointer       user_data)
{
  PanedState *state = user_data;
  gint pos = gtk_paned_get_position (paned);

  if (!state->dragging || pos < MIN_POSITION || pos == state->position)
    return;

  state->position = pos;
  g_clear_handle_id (&state->save_handler, g_source_remove);
  state->save_handler = g_timeout_add_seconds_once (1, on_save_timeout, state);
}

static gboolean
on_tick (GtkWidget           *widget,
         GdkFrameClock *clock G_GNUC_UNUSED,
         gpointer             user_data)
{
  PanedState *state = user_data;
  gint width = gtk_widget_get_width (widget);

  if (width > 0 && width != state->last_width) {
    state->last_width = width;

    if (!state->dragging && gtk_paned_get_position (state->paned) != state->position)
      gtk_paned_set_position (state->paned, state->position);
  }

  return G_SOURCE_CONTINUE;
}

static void
on_unmap (GtkWidget *widget G_GNUC_UNUSED,
          gpointer          user_data)
{
  PanedState *state = user_data;

  state->last_width = 0;
}

void
stamp_paned_remember_position (GtkPaned    *paned,
                               const gchar *settings_key)
{
  PanedState *state = g_new0 (PanedState, 1);
  GtkEventController *controller = gtk_event_controller_legacy_new ();

  state->paned = paned;
  state->settings_key = g_strdup (settings_key);
  state->position = CLAMP (g_settings_get_int (STAMP_SETTINGS, settings_key), 250, 2000);
  g_object_set_data_full (G_OBJECT (paned), "stamp-paned-state", state, (GDestroyNotify)paned_state_free);

  gtk_paned_set_position (paned, state->position);

  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "event", G_CALLBACK (on_event), state);
  gtk_widget_add_controller (GTK_WIDGET (paned), controller);
  gtk_widget_add_tick_callback (GTK_WIDGET (paned), on_tick, state, NULL);
  g_signal_connect (paned, "notify::position", G_CALLBACK (on_position_changed), state);
  g_signal_connect (paned, "unmap", G_CALLBACK (on_unmap), state);
}
