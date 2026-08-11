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

#pragma once

#include <adwaita.h>
#include <camel/camel.h>

#include "stamp-account.h"
#include "stamp-webview.h"

G_BEGIN_DECLS

#define STAMP_TYPE_COMPOSER (stamp_composer_get_type ())
G_DECLARE_FINAL_TYPE (StampComposer, stamp_composer, STAMP, COMPOSER, AdwApplicationWindow);

typedef enum {
  STAMP_COMPOSER_NEW,
  STAMP_COMPOSER_REPLY,
  STAMP_COMPOSER_REPLY_ALL,
  STAMP_COMPOSER_FORWARD,
  STAMP_COMPOSER_DRAFT,
} StampComposerType;

GtkWidget *stamp_composer_new (StampAccount *account);

GtkWidget *stamp_composer_new_with_quote (StampComposerType       type,
                                          const gchar             *uid,
                                          StampAccount           *account,
                                          StampWebView           *webview,
                                          const CamelMessageInfo *info,
                                          CamelMimeMessage       *mime_message,
                                          gchar                   *content_to_quote);

void stamp_composer_set_to (StampComposer *self,
                            gchar          *to);

void
stamp_composer_set_subject (StampComposer *self,
                            gchar          *subject);

void
stamp_composer_set_body (StampComposer *self,
                         gchar          *body);

void
stamp_composer_add_attachment (StampComposer *self,
                               gchar         *path);

G_END_DECLS

