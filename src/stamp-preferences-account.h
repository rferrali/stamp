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

#include <adwaita.h>
#include <gtk/gtk.h>

#include "stamp-account.h"
#include "stamp-signature-store.h"

#pragma once

G_BEGIN_DECLS

#define STAMP_TYPE_PREFERENCES_ACCOUNT (stamp_preferences_account_get_type ())

G_DECLARE_FINAL_TYPE (StampPreferencesAccount, stamp_preferences_account, STAMP, PREFERENCES_ACCOUNT, AdwNavigationPage);

#define STAMP_TYPE_PREFERENCES_ACCOUNT_EDITOR (stamp_preferences_account_editor_get_type ())

G_DECLARE_FINAL_TYPE (StampPreferencesAccountEditor, stamp_preferences_account_editor, STAMP, PREFERENCES_ACCOUNT_EDITOR, AdwNavigationPage);

GtkWidget *
stamp_preferences_account_new (StampAccount        *account,
                               StampSignatureStore *signature_store);

GtkWidget *
stamp_preferences_account_editor_new (StampAccount *account, const gchar *email, const gchar *name);

G_END_DECLS