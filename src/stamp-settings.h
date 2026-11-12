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

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

#define STAMP_PREFS_SCHEMA                    "org.tabos.stamp"
#define STAMP_PREFS_WINDOW_MAXIMIZED          "window-maximized"
#define STAMP_PREFS_WINDOW_SIZE               "window-size"
#define STAMP_PREFS_WINDOW_WIDTH              "window-width"
#define STAMP_PREFS_WINDOW_HEIGHT             "window-height"
#define STAMP_PREFS_MAIL_DESKTOP_LIST_PANE_WIDTH      "mail-desktop-list-pane-width"
#define STAMP_PREFS_MAIL_TABLET_LIST_PANE_WIDTH       "mail-tablet-list-pane-width"
#define STAMP_PREFS_CONTACT_DESKTOP_LIST_PANE_WIDTH   "contact-desktop-list-pane-width"
#define STAMP_PREFS_CONTACT_TABLET_LIST_PANE_WIDTH    "contact-tablet-list-pane-width"
#define STAMP_PREFS_BACKGROUND_NOTIFICATIONS  "background-notifications"
#define STAMP_PREFS_BACKGROUND_AUTOSTART      "autostart"
#define STAMP_PREFS_VIEW                      "view"
#define STAMP_PREFS_ACCOUNT_ORDER             "account-order"
#define STAMP_PREFS_COMPOSER_MAXIMIZED        "composer-maximized"
#define STAMP_PREFS_COMPOSER_WIDTH            "composer-width"
#define STAMP_PREFS_COMPOSER_HEIGHT           "composer-height"

#define STAMP_PREFS_MAIL_SCHEMA               "org.tabos.stamp.mail"
#define STAMP_PREFS_MAIL_SELECTED_FOLDER      "selected-folder"
#define STAMP_PREFS_MAIL_ALL_MAILBOXES_EXPANDED "all-mailboxes-expanded"
#define STAMP_PREFS_MAIL_ALWAYS_SHOW_IMAGES   "always-show-images"
#define STAMP_PREFS_MAIL_LOAD_BIMI_IMAGES     "load-bimi-images"
#define STAMP_PREFS_MAIL_MARK_READ_TIMEOUT    "mark-read-timeout"
#define STAMP_PREFS_MAIL_PLAY_INCOMING_SOUND  "play-incoming-sound"
#define STAMP_PREFS_MAIL_IMPORTANT_FIRST      "important-first"
#define STAMP_PREFS_MAIL_VERTICAL_VIEW        "vertical-mail-view"
#define STAMP_PREFS_MAIL_REFRESH_INTERVAL     "refresh-interval"
#define STAMP_PREFS_MAIL_SWIPE_RIGHT_ACTION   "swipe-right-action"
#define STAMP_PREFS_MAIL_SWIPE_LEFT_ACTION    "swipe-left-action"

#define STAMP_PREFS_ACCOUNTS_SCHEMA            "org.tabos.stamp.accounts"
#define STAMP_PREFS_MAIL_DEFAULT_SIGNATURE     "default-signature"

#define STAMP_SETTINGS stamp_settings_get (STAMP_PREFS_SCHEMA)
#define STAMP_SETTINGS_MAIL stamp_settings_get (STAMP_PREFS_MAIL_SCHEMA)
#define STAMP_SETTINGS_ACCOUNTS stamp_settings_get (STAMP_PREFS_ACCOUNTS_SCHEMA)

GSettings *
stamp_settings_get (const gchar *schema);

void
stamp_settings_init (void);

void
stamp_settings_shutdown (void);

G_END_DECLS

