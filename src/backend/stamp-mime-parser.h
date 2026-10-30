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

#include <camel/camel.h>
#include <glib.h>

#define LIBICAL_GLIB_UNSTABLE_API 1
#include <libical-glib/libical-glib.h>

/* Ensure G_GNUC_FLAG_ENUM is defined (glib < 2.88) */
#ifndef G_GNUC_FLAG_ENUM
#define G_GNUC_FLAG_ENUM __attribute__((__flag_enum__))
#endif

G_BEGIN_DECLS

typedef struct _ICalComponent ICalComponent;

typedef struct _StampMimeParser StampMimeParser;

typedef enum {
  STAMP_MIME_SIGNATURE_NONE,
  STAMP_MIME_SIGNATURE_UNKNOWN,
  STAMP_MIME_SIGNATURE_GOOD,
  STAMP_MIME_SIGNATURE_BAD,
} StampMimeSignatureStatus;

typedef enum {
  STAMP_MIME_ENCRYPTION_NONE,
  STAMP_MIME_ENCRYPTION_VALID,
  STAMP_MIME_ENCRYPTION_INVALID,
} StampMimeEncryptionStatus;

typedef enum {
  STAMP_MIME_ATTACHMENT_GENERIC = 0x01,
  STAMP_MIME_ATTACHMENT_IMAGE = 0x02,
  STAMP_MIME_ATTACHMENT_CALENDAR = 0x04,
  STAMP_MIME_ATTACHMENT_PGP_KEY = 0x08,
  STAMP_MIME_ATTACHMENT_INVITATION = 0x10,
  STAMP_MIME_ATTACHMENT_MESSAGE = 0x20,
} G_GNUC_FLAG_ENUM StampMimeAttachmentKind;

typedef struct {
  gchar *name;
  gchar *email;
} StampMimeSignerInfo;

typedef struct {
  StampMimeSignatureStatus status;
  GList *signers;
  gchar *description;
  gboolean is_smime;
  gboolean is_inline;
} StampMimeSignature;

typedef enum {
  STAMP_MIME_PGP_INLINE_NONE       = 0,
  STAMP_MIME_PGP_INLINE_SIGNED     = 1,
  STAMP_MIME_PGP_INLINE_ENCRYPTED  = 2,
} StampMimePgpInlineType;

typedef struct {
  StampMimeEncryptionStatus status;
  gchar *error_message;
  gboolean success;
  gboolean is_smime;
  gboolean is_inline;
} StampMimeEncryption;

typedef struct {
  gchar *text;
  gsize length;
  gchar *charset;
  gboolean is_html;

  gboolean is_signed;
  gboolean is_encrypted;
} StampMimeBody;

typedef struct {
  gchar *filename;
  gchar *mime_type;
  gchar *disposition;
  GBytes *data;
  gsize size;
  gboolean is_inline;
  StampMimeAttachmentKind  kind;
  gchar *content_id;
  gchar *image_format;
  gchar *calendar_method;
} StampMimeAttachment;

typedef struct {
  gchar *content_id;
  gchar *content_type;
  GBytes *data;
} StampMimeInlinePart;

typedef struct {
  gchar *uid;
  gchar *method;
  ICalComponent *ical;
} StampMimeCalendar;

typedef enum {
  STAMP_MIME_UNSUBSCRIBE_MAILTO = 1,
  STAMP_MIME_UNSUBSCRIBE_HTTP   = 2,
} StampMimeUnsubscribeMethod;

typedef struct {
  StampMimeUnsubscribeMethod method;
  gchar *uri;
  gchar *mailto;
  gboolean one_click;
} StampMimeListUnsubscribe;

typedef void (*StampMimeParserCallback) (StampMimeParser *self,
                                        gpointer user_data);

#define STAMP_TYPE_MIME_PARSER (stamp_mime_parser_get_type())
G_DECLARE_FINAL_TYPE(StampMimeParser, stamp_mime_parser, STAMP, MIME_PARSER, GObject);

StampMimeParser *
stamp_mime_parser_new (CamelSession *session);

gboolean
stamp_mime_parser_parse (StampMimeParser   *self,
                         CamelMimeMessage  *message,
                         GCancellable      *cancellable,
                         GError           **error);

void
stamp_mime_parser_parse_async (StampMimeParser     *self,
                               CamelMimeMessage    *message,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data);

gboolean
stamp_mime_parser_parse_finish (StampMimeParser  *self,
                                GAsyncResult     *result,
                                GError          **error);

StampMimeBody *
stamp_mime_parser_get_body (StampMimeParser *self);

GList *
stamp_mime_parser_get_attachments (StampMimeParser *self);

const GList *
stamp_mime_parser_get_signatures (StampMimeParser *self);

const GList *
stamp_mime_parser_get_encryptions (StampMimeParser *self);

GList *
stamp_mime_parser_get_inline_images (StampMimeParser *self);

GList *
stamp_mime_parser_get_list_unsubscribe (StampMimeParser *self);

void
stamp_mime_parser_send_unsubscribe (StampMimeParser          *parser,
                                    StampMimeListUnsubscribe *unsubscribe,
                                    GCancellable             *cancellable);

StampMimeCalendar *
stamp_mime_parser_get_invitations (StampMimeParser *self);

G_END_DECLS
