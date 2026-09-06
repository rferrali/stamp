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

#include <camel/camel.h>
#include <gdk/gdk.h>
#include <glib-object.h>
#include <libebook/libebook.h>
#include <libedataserverui4/libedataserverui4.h>

#include "stamp-category.h"

G_BEGIN_DECLS

#define STAMP_TYPE_ACCOUNT (stamp_account_get_type ())

G_DECLARE_FINAL_TYPE (StampAccount, stamp_account, STAMP, ACCOUNT, GObject);

typedef struct _StampMailService StampMailService;
typedef struct _StampContactsService StampContactsService;
typedef struct _StampCalendarService StampCalendarService;

typedef enum {
  STAMP_FOLDER_TYPE_INBOX = 0,
  STAMP_FOLDER_TYPE_STARRED,
  STAMP_FOLDER_TYPE_IMPORTANT,
  STAMP_FOLDER_TYPE_OUTBOX,
  STAMP_FOLDER_TYPE_DRAFTS,
  STAMP_FOLDER_TYPE_SENT,
  STAMP_FOLDER_TYPE_JUNK,
  STAMP_FOLDER_TYPE_TRASH,
  STAMP_FOLDER_TYPE_ARCHIVES,
  STAMP_FOLDER_TYPE_TEMPLATES,
  STAMP_FOLDER_TYPE_NORMAL,
} StampFolderType;

/*
 * Setup
 */

StampAccount *
stamp_account_new (ESource         *collection,
                   ESourceRegistry *registry);

void
stamp_account_add_mail (StampAccount *self,
                        ESource      *source);

void
stamp_account_add_address_book (StampAccount *self,
                                ESource      *source);

void
stamp_account_add_mail_transport (StampAccount *self,
                                  ESource      *source);

void
stamp_account_add_mail_identity (StampAccount *self,
                                 ESource      *identity);

void
stamp_account_init_async (StampAccount        *self,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data);

gboolean
stamp_account_init_finish (GAsyncResult  *res,
                           GError       **error);

void
stamp_account_clear_negative_photo_cache (StampAccount *self);

/*
 * Getter/Setter
 */

const gchar *
stamp_account_get_name (StampAccount *self);

void
stamp_account_set_name (StampAccount *self,
                        const gchar   *name);

const gchar *
stamp_account_get_uid (StampAccount *self);

const gchar *
stamp_account_get_color_class (StampAccount *self);

void
stamp_account_set_badge_color (StampAccount *self,
                               const gchar  *badge_color);

gchar *
stamp_account_get_badge_color (StampAccount *self);

void
stamp_account_update_color (StampAccount *self);

CamelInternetAddress *
stamp_account_get_address (StampAccount *self);

void
stamp_account_contacts_changed (StampAccount *self,
                                ESource      *source);

void
stamp_account_mail_changed (StampAccount *self,
                            ESource      *source);

GPtrArray *
stamp_account_get_books (StampAccount *self);

StampMailService *
stamp_account_get_mail_service (StampAccount *self);

/*
 * Mail Service Getter
 */

CamelService *
stamp_mail_service_get_service (StampMailService *self);

CamelTransport *
stamp_mail_service_get_transport (StampMailService *self);

gboolean
stamp_mail_service_get_enabled (StampMailService *self);

ESource *
stamp_mail_service_get_source (StampMailService *self);

ESource *
stamp_mail_service_get_transport_source (StampMailService *self);

ESource *
stamp_mail_service_get_identity_source (StampMailService *self);

/*
 * Contacts
 */

void
stamp_account_get_photo (StampAccount        *self,
                         const gchar          *sender,
                         GCancellable        *cancellable,
                         GFunc  callback,
                         gpointer             user_data);

gpointer
stamp_account_get_photo_finish (StampAccount  *self,
                                GAsyncResult  *result,
                                GError       **error);

void
stamp_account_search_contacts (StampAccount        *self,
                               EBookClient         *client,
                               const gchar          *search_text,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data);

GSList *
stamp_account_search_contacts_finish (StampAccount  *self,
                                      EBookClient   *client,
                                      GAsyncResult  *res,
                                      GError       **error);

void
stamp_account_search_conversations (StampAccount        *self,
                                    const gchar          *search_text,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);

GSList *
stamp_account_search_conversations_finish (StampAccount  *self,
                                          GAsyncResult  *res,
                                          GError       **error);

/*
 * Mail
 */

void
stamp_account_send_mail (StampAccount         *self,
                         CamelMimeMessage     *message,
                         CamelInternetAddress *sender,
                         CamelInternetAddress *recipient,
                         gboolean              pgp_sign,
                         gboolean              pgp_encrypt,
                         GCancellable         *cancellable,
                         GAsyncReadyCallback   callback,
                         gpointer              user_data);

gpointer
stamp_account_send_mail_finish (StampAccount  *session,
                                GAsyncResult  *result,
                                GError       **error);

gchar *
stamp_account_save_draft (StampAccount         *self,
                          const gchar           *draft_uid,
                          CamelMimeMessage     *message,
                          CamelInternetAddress *sender,
                          CamelInternetAddress *recipient);

void
stamp_account_save_draft_async (StampAccount         *self,
                                const gchar           *draft_uid,
                                CamelMimeMessage     *message,
                                CamelInternetAddress *sender,
                                CamelInternetAddress *recipient,
                                GCancellable         *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data);

gchar *
stamp_account_save_draft_async_finish (StampAccount  *self,
                                       GAsyncResult  *result,
                                       GError       **error);

void
stamp_account_remove_draft (StampAccount *self,
                            const gchar   *uid);

void
stamp_account_remove_draft_async (StampAccount        *self,
                                 const gchar           *uid,
                                 GCancellable         *cancellable,
                                 GAsyncReadyCallback   callback,
                                 gpointer             user_data);

gboolean
stamp_account_remove_draft_async_finish (StampAccount  *self,
                                        GAsyncResult  *result,
                                        GError       **error);

CamelFolder *
stamp_account_get_mail_trash_folder (StampAccount *self);

CamelFolder *
stamp_account_get_mail_drafts_folder (StampAccount *self);

CamelFolder *
stamp_account_get_mail_sent_folder (StampAccount *self);

CamelFolder *
stamp_account_get_mail_junk_folder (StampAccount *self);

CamelFolder *
stamp_account_get_mail_archives_folder (StampAccount *self);

CamelFolder *
stamp_account_get_mail_templates_folder (StampAccount *self);

GList *
stamp_account_get_categories (StampAccount *self);

gint
stamp_account_get_folder_order_rank (StampAccount    *self,
                                     CamelFolderInfo *info);

StampCategory *
stamp_account_find_category (StampAccount *self,
                             const gchar   *name);

GHashTable *
stamp_mail_service_get_aliases (StampMailService *self);

void
stamp_mail_service_set_aliases (StampMailService *self,
                         GHashTable      *aliases);

/*
 * Contacts Service Getter
 */

gboolean
stamp_contacts_service_get_enabled (StampContactsService *self);

EBookClient *
stamp_contacts_service_get_client (StampContactsService *self);

ESource *
stamp_contacts_service_get_source (StampContactsService *self);

G_END_DECLS

