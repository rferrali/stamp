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

#include "stamp-mime-parser.h"

#include <libecal/libecal.h>
#include <libsoup/soup.h>

struct _StampMimeParser {
  GObject parent_instance;

  CamelSession *session;

  StampMimeBody *html_body;
  StampMimeBody *plain_body;
  StampMimeCalendar *calendar;

  GList *attachments;
  GList *signatures;
  GList *encryption;

  GList *list_unsubscribe;

  gboolean found_encrypted;
  gboolean unsubscribe_one_click;

  guint depth;
};

G_DEFINE_FINAL_TYPE (StampMimeParser, stamp_mime_parser, G_TYPE_OBJECT);

#define PARSER_LOG(fmt, ...) \
        g_debug ("%*s%s " fmt, self->depth, "", G_STRFUNC,  ## __VA_ARGS__);

typedef gboolean (*StampPartHandlerFunc)(StampMimeParser *self,
                                         CamelMimePart   *part,
                                         gconstpointer    user_data,
                                         GCancellable    *cancellable,
                                         GError         **error);

typedef struct {
  const gchar *type;
  const gchar *subtype;
  StampPartHandlerFunc handler;
  gconstpointer user_data;
} StampPartDispatchEntry;

static gboolean dispatch_part (StampMimeParser *self,
                               CamelMimePart   *part,
                               GCancellable    *cancellable,
                               GError         **error);

static void
stamp_mime_body_free (StampMimeBody *body)
{
  g_assert (body);

  g_clear_pointer (&body->text, g_free);
  g_clear_pointer (&body->charset, g_free);
  g_clear_pointer (&body, g_free);
}

static StampMimeBody *
stamp_mime_body_new (void)
{
  return g_new0 (StampMimeBody, 1);
}

static void
stamp_mime_attachment_free (StampMimeAttachment *attachment)
{
  g_assert (attachment);

  g_clear_pointer (&attachment->filename, g_free);
  g_clear_pointer (&attachment->mime_type, g_free);
  g_clear_pointer (&attachment->disposition, g_free);
  g_clear_pointer (&attachment->data, g_bytes_unref);
  g_clear_pointer (&attachment, g_free);
}

static StampMimeAttachment *
stamp_mime_attachment_new (void)
{
  return g_new0 (StampMimeAttachment, 1);
}

static void
stamp_mime_list_unsubscribe_free (StampMimeListUnsubscribe *list_unsubscribe)
{
  g_assert (list_unsubscribe);

  g_clear_pointer (&list_unsubscribe->uri, g_free);
  g_clear_pointer (&list_unsubscribe->mailto, g_free);
  g_clear_pointer (&list_unsubscribe, g_free);
}

static StampMimeListUnsubscribe *
stamp_mime_list_unsubscribe_new (void)
{
  return g_new0 (StampMimeListUnsubscribe, 1);
}

static void
stamp_mime_calendar_free (StampMimeCalendar *calendar)
{
  g_assert (calendar);

  g_clear_pointer (&calendar->uid, g_free);
  g_clear_pointer (&calendar->method, g_free);
  g_clear_object (&calendar->ical);
  g_clear_pointer (&calendar, g_free);
}

static StampMimeCalendar *
stamp_mime_calendar_new (void)
{
  return g_new0 (StampMimeCalendar, 1);
}

static void
stamp_mime_encryption_free (StampMimeEncryption *encryption)
{
  g_free (encryption->error_message);
  g_free (encryption);
}

static void
stamp_mime_signature_free (StampMimeSignature *signature)
{
  for (GList *iter = signature->signers; iter; iter = iter->next) {
    StampMimeSignerInfo *info = iter->data;

    g_free (info->name);
    g_free (info->email);
    g_free (info);
  }
  g_list_free (signature->signers);
  g_free (signature->description);
  g_free (signature);
}

static void
stamp_mime_parser_clear (StampMimeParser *self)
{
  g_clear_pointer (&self->html_body, stamp_mime_body_free);
  g_clear_pointer (&self->plain_body, stamp_mime_body_free);

  g_clear_list (&self->signatures, (GDestroyNotify)stamp_mime_signature_free);
  g_clear_list (&self->encryption, (GDestroyNotify)stamp_mime_encryption_free);
  g_clear_list (&self->attachments, (GDestroyNotify)stamp_mime_attachment_free);
  g_clear_list (&self->list_unsubscribe, (GDestroyNotify)stamp_mime_list_unsubscribe_free);

  self->found_encrypted = FALSE;
  self->unsubscribe_one_click = FALSE;
  self->depth = 0;
}

static void
stamp_mime_parser_dispose (GObject *object)
{
  StampMimeParser *self = STAMP_MIME_PARSER (object);

  g_clear_object (&self->session);
  stamp_mime_parser_clear (self);

  G_OBJECT_CLASS (stamp_mime_parser_parent_class)->dispose (object);
}

static void
stamp_mime_parser_class_init (StampMimeParserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = stamp_mime_parser_dispose;
}

static void
stamp_mime_parser_init (StampMimeParser *self)
{
}

StampMimeParser *
stamp_mime_parser_new (CamelSession *session)
{
  StampMimeParser *self;

  g_return_val_if_fail (CAMEL_IS_SESSION (session), NULL);

  self = g_object_new (STAMP_TYPE_MIME_PARSER, NULL);
  self->session = g_object_ref (session);

  return self;
}
static void
convert_newlines_to_br (gchar **content)
{
  const gchar *p;
  g_autoptr (GString) out = NULL;
  gboolean changed = FALSE;

  if (!*content)
    return;

  out = g_string_new (NULL);

  for (p = *content; *p; p++) {
    if (*p == '\n') {
      g_string_append (out, "<br/>");
      changed = TRUE;
    } else if (*p == '\r') {
      if (p[1] == '\n') {
        g_string_append (out, "<br/>");
        p++;
        changed = TRUE;
      } else {
        g_string_append_c (out, *p);
      }
    } else {
      g_string_append_c (out, *p);
    }
  }

  if (changed) {
    g_free (*content);
    *content = g_string_free_and_steal (g_steal_pointer (&out));
  }
}

static GByteArray *
decode_part_to_bytes (CamelMimePart  *part,
                      GCancellable   *cancellable,
                      GError        **error)
{
  CamelDataWrapper *data_wrapper = camel_medium_get_content (CAMEL_MEDIUM (part));
  g_autoptr (CamelStream) out = NULL;
  GByteArray *result = NULL;
  gboolean ok;

  if (!data_wrapper)
    return NULL;

  out = camel_stream_mem_new ();
  ok = camel_data_wrapper_decode_to_stream_sync (data_wrapper, out, cancellable, error) >= 0;
  if (ok) {
    GByteArray *internal = camel_stream_mem_get_byte_array (CAMEL_STREAM_MEM (out));

    if (internal && internal->len > 0) {
      result = g_byte_array_new_take (internal->data, internal->len);
      internal->data = NULL;
      internal->len = 0;
    } else {
      result = g_byte_array_new ();
    }
  }

  return result;
}

static gchar *
convert_to_utf8 (guint8      *data,
                 gsize        len,
                 const gchar *charset,
                 gsize       *out_len)
{
  gsize written = 0;
  g_autoptr (GError) err = NULL;
  gchar *utf8;

  if (!data || len == 0) {
    if (out_len)
      *out_len = 0;

    return g_strdup ("");
  }

  if (!charset || g_ascii_strcasecmp (charset, "utf-8") == 0 || g_ascii_strcasecmp (charset, "utf8") == 0) {
    gchar *valid;

    if (g_utf8_validate ((const char *)data, (gssize)len, NULL)) {
      data = g_realloc (data, len + 1);
      data[len] = '\0';

      if (out_len)
        *out_len = len;

      return (char *)data;
    }

    valid = g_utf8_make_valid ((const char *)data, (gssize)len);
    g_free (data);

    if (out_len)
      *out_len = valid ? strlen (valid) : 0;

    return valid;
  }

  utf8 = g_convert ((const char *)data, (gssize)len, "UTF-8", charset, NULL, &written, &err);
  g_free (data);

  if (err) {
    g_warning ("%s: Error during charset conversion '%s': %s", G_STRFUNC, charset, err->message);

    if (out_len)
      *out_len = 0;

    return g_strdup ("");
  }

  if (out_len)
    *out_len = written;

  return utf8;
}

static StampMimeSignatureStatus
map_validity_status (CamelCipherValiditySign status)
{
  switch (status) {
    case CAMEL_CIPHER_VALIDITY_SIGN_GOOD:
      return STAMP_MIME_SIGNATURE_GOOD;
    case CAMEL_CIPHER_VALIDITY_SIGN_BAD:
      return STAMP_MIME_SIGNATURE_BAD;
    case CAMEL_CIPHER_VALIDITY_SIGN_NONE:
      return STAMP_MIME_SIGNATURE_NONE;
    case CAMEL_CIPHER_VALIDITY_SIGN_NEED_PUBLIC_KEY:
    case CAMEL_CIPHER_VALIDITY_SIGN_UNKNOWN:
    default:
      return STAMP_MIME_SIGNATURE_UNKNOWN;
  }
}

static gboolean
is_email_char (gchar c)
{
  return g_ascii_isalnum (c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
}

static gboolean
is_domain_char (gchar c)
{
  return g_ascii_isalnum (c) || c == '.' || c == '-';
}

static gboolean
is_scheme (const gchar *p)
{
  if (*p != 'h' && *p != 'f' && *p != 'g')
    return FALSE;

  return strncmp (p, "http://", 7) == 0
         || strncmp (p, "https://", 8) == 0
         || strncmp (p, "ftp://", 6) == 0
         || strncmp (p, "git://", 6) == 0;
}

static gboolean
is_trailing_url_char (gchar c)
{
  return c == '.' || c == ',' || c == ';' || c == ':' || c == '!'
         || c == '?' || c == ')' || c == '\'' || c == '"';
}

static gchar *
linkify_urls (const gchar *text)
{
  GString *out = g_string_new (NULL);
  const gchar *p = text;

  while (*p) {
    if (is_scheme (p)) {
      const gchar *start = p;
      const gchar *end = p;

      while (*end && !g_ascii_isspace (*end) && *end != '<' && *end != '>' && *end != '"')
        end++;

      while (end > start && is_trailing_url_char (end[-1]))
        end--;

      if (end > start + 7) {
        g_autofree char *url = g_strndup (start, end - start);

        g_string_append_printf (out, "<a href=\"%s\">%s</a>", url, url);
        p = end;
        continue;
      }
    }

    g_string_append_c (out, *p);
    p++;
  }

  return g_string_free_and_steal (out);
}

static gchar *
linkify_emails (const gchar *text)
{
  GString *out = g_string_new (NULL);
  const gchar *p = text;

  while (*p) {
    if (*p == '@') {
      const gchar *local_start = p;
      const gchar *domain = p + 1;
      const gchar *domain_end = domain;
      const gchar *last_dot = NULL;

      while (local_start > text && is_email_char (local_start[-1]))
        local_start--;

      while (*domain_end && is_domain_char (*domain_end)) {
        if (*domain_end == '.')
          last_dot = domain_end;
        domain_end++;
      }

      if (local_start < p && last_dot && last_dot + 2 <= domain_end) {
        gboolean tld_ok = TRUE;
        const gchar *t;

        for (t = last_dot + 1; t < domain_end; t++) {
          if (!g_ascii_isalpha (*t)) {
            tld_ok = FALSE;
            break;
          }
        }

        if (tld_ok) {
          g_autofree char *email = g_strndup (local_start, domain_end - local_start);

          g_string_truncate (out, out->len - (p - local_start));
          g_string_append_printf (out, "<a href=\"mailto:%s\">%s</a>", email, email);
          p = domain_end;
          continue;
        }
      }
    }

    g_string_append_c (out, *p);
    p++;
  }

  return g_string_free_and_steal (out);
}

static void
convert_links (StampMimeParser *self)
{
  g_autofree char *linked = NULL;
  g_autofree char *with_emails = NULL;
  g_autofree char *wrapped = NULL;
  g_autofree char *escaped = NULL;

  if (!self->plain_body->text)
    return;

  linked = linkify_urls (self->plain_body->text);
  with_emails = linkify_emails (linked);
  escaped = g_markup_escape_text (with_emails, -1);

  wrapped = g_strdup_printf ("<pre>%s</pre>", escaped);
  g_set_str (&self->plain_body->text, wrapped);
  self->plain_body->length = strlen (self->plain_body->text);
}

#define STAMP_PGP_ARMOR_SIGNED     "-----BEGIN PGP SIGNED MESSAGE-----"
#define STAMP_PGP_ARMOR_ENCRYPTED  "-----BEGIN PGP MESSAGE-----"
#define STAMP_PGP_ARMOR_END_SIG    "-----END PGP SIGNATURE-----"
#define STAMP_PGP_ARMOR_END_MSG    "-----END PGP MESSAGE-----"

static StampMimePgpInlineType
detect_pgp_inline (const gchar *text)
{
  const gchar *anchor;

  if (!text)
    return STAMP_MIME_PGP_INLINE_NONE;

  anchor = strstr (text, "-----BEGIN PGP ");
  if (!anchor)
    return STAMP_MIME_PGP_INLINE_NONE;

  if (strncmp (anchor, STAMP_PGP_ARMOR_SIGNED, strlen (STAMP_PGP_ARMOR_SIGNED)) == 0)
    return STAMP_MIME_PGP_INLINE_SIGNED;

  if (strncmp (anchor, STAMP_PGP_ARMOR_ENCRYPTED, strlen (STAMP_PGP_ARMOR_ENCRYPTED)) == 0)
    return STAMP_MIME_PGP_INLINE_ENCRYPTED;

  return STAMP_MIME_PGP_INLINE_NONE;
}

static gchar *
extract_pgp_cleartext (const gchar *text)
{
  const gchar *begin = strstr (text, STAMP_PGP_ARMOR_SIGNED);
  const gchar *body_start;
  const gchar *sig_start;
  gsize len;

  if (!begin)
    return NULL;

  body_start = strstr (begin, "\n\n");
  if (!body_start)
    body_start = strstr (begin, "\r\n\r\n");

  if (!body_start)
    return NULL;

  body_start += 2;
  if (*body_start == '\n')
    body_start++;

  sig_start = strstr (body_start, "-----BEGIN PGP SIGNATURE-----");
  if (!sig_start)
    return NULL;

  len = sig_start - body_start;
  if (len > 0 && body_start[len - 1] == '\n')
    len--;

  if (len > 0 && body_start[len - 1] == '\r')
    len--;

  return g_strndup (body_start, len);
}

static GList *
build_signer_list (CamelCipherValidity *validity)
{
  GList *head = g_queue_peek_head_link (&validity->sign.signers);
  GList *result = NULL;

  for (GList *l = head; l; l = l->next) {
    CamelCipherCertInfo *ci = l->data;
    StampMimeSignerInfo *info = g_new0 (StampMimeSignerInfo, 1);

    info->name = g_strdup (ci->name);
    info->email = g_strdup (ci->email);

    result = g_list_append (result, info);
  }

  return result;
}

static gboolean
handle_pgp_inline (StampMimeParser         *self,
                   CamelMimePart           *part,
                   const gchar             *text,
                   StampMimePgpInlineType   pgp_type,
                   GCancellable            *cancellable,
                   GError                 **error)
{
  g_autoptr (CamelCipherContext) cipher = camel_gpg_context_new (self->session);

  if (!cipher)
    return FALSE;

  if (pgp_type == STAMP_MIME_PGP_INLINE_SIGNED) {
    StampMimeSignature *signature;
    g_autoptr (GError) cipher_err = NULL;
    g_autoptr (CamelCipherValidity) validity = camel_cipher_context_verify_sync (cipher, CAMEL_MIME_PART (part), cancellable, &cipher_err);
    gchar *cleartext;

    signature = g_new0 (StampMimeSignature, 1);
    signature->is_smime = FALSE;
    signature->is_inline = TRUE;

    if (cipher_err) {
      signature->status = STAMP_MIME_SIGNATURE_UNKNOWN;
      signature->description = g_strdup (cipher_err->message);
    } else if (validity) {
      signature->status = map_validity_status (validity->sign.status);
      signature->description = g_strdup (validity->sign.description);
      signature->signers = build_signer_list (validity);
    }

    self->signatures = g_list_append (self->signatures, signature);

    cleartext = extract_pgp_cleartext (text);
    if (cleartext) {
      CamelContentType *ct = camel_mime_part_get_content_type (part);
      StampMimeBody *body;
      const gchar *charset = camel_content_type_param (ct, "charset");

      body = g_new0 (StampMimeBody, 1);
      body->is_html = FALSE;
      body->text = cleartext;
      body->length = strlen (cleartext);
      body->charset = g_strdup (charset);

      convert_newlines_to_br (&body->text);
      body->length = strlen (body->text);

      g_clear_pointer (&self->plain_body, stamp_mime_body_free);
      self->plain_body = body;
    }
  } else {
    StampMimeEncryption *encryption;
    g_autoptr (GError) dec_err = NULL;
    g_autoptr (CamelMimePart) out = camel_mime_part_new ();
    g_autoptr (CamelCipherValidity) cv = NULL;

    self->found_encrypted = TRUE;

    encryption = g_new0 (StampMimeEncryption, 1);
    encryption->is_smime = FALSE;
    encryption->is_inline = TRUE;

    cv = camel_cipher_context_decrypt_sync (cipher, CAMEL_MIME_PART (part), out, cancellable, &dec_err);
    if (!cv || dec_err) {
      encryption->success = FALSE;
      encryption->status = STAMP_MIME_ENCRYPTION_INVALID;
      encryption->error_message = dec_err ? g_strdup (dec_err->message) : g_strdup ("Inline PGP decryption failed");
    } else {
      g_autoptr (GError) child_err = NULL;
      encryption->success = TRUE;
      encryption->status = STAMP_MIME_ENCRYPTION_VALID;

      dispatch_part (self, out, cancellable, &child_err);
    }

    self->encryption = g_list_append (self->encryption, encryption);
  }

  return TRUE;
}

static void
handle_text_part (StampMimeParser *self,
                  CamelMimePart   *part,
                  gboolean         is_html,
                  GCancellable    *cancellable)
{
  CamelContentType *content_type = camel_mime_part_get_content_type (part);
  const gchar *charset = camel_content_type_param (content_type, "charset");
  GByteArray *ba = decode_part_to_bytes (part, cancellable, NULL);
  gsize raw_len;
  guint8 *raw;
  gsize text_len = 0;
  g_autofree char *text = NULL;
  StampMimeBody *bp;

  if (!ba)
    return;

  raw_len = ba->len;
  raw = g_byte_array_free (ba, FALSE);
  text = convert_to_utf8 (raw, raw_len, charset, &text_len);

  if (!text)
    return;

  /* Inline PGP check only in text/plain */
  if (!is_html) {
    StampMimePgpInlineType pgp = detect_pgp_inline (text);

    if (pgp != STAMP_MIME_PGP_INLINE_NONE) {
      handle_pgp_inline (self, part, text, pgp, cancellable, NULL);
      return;
    }
  }

  bp = stamp_mime_body_new ();
  bp->is_html = is_html;
  bp->text = g_steal_pointer (&text);
  bp->length = text_len;
  bp->charset = g_strdup (charset);

  if (is_html) {
    g_clear_pointer (&self->html_body, stamp_mime_body_free);
    self->html_body = bp;
  } else if (!self->plain_body) {
    g_clear_pointer (&self->plain_body, stamp_mime_body_free);
    self->plain_body = bp;

    convert_links (self);
  }
}

static gchar *
image_format_from_subtype (const gchar *subtype)
{
  if (!subtype)
    return g_strdup ("unknown");

  if (g_strcmp0 (subtype, "jpg") == 0)
    return g_strdup ("jpeg");

  return g_strdup (subtype);
}

static gchar *
calendar_method_from_part (CamelMimePart *part)
{
  CamelContentType *ct = camel_mime_part_get_content_type (part);
  const gchar *method;

  if (!ct)
    return NULL;

  method = camel_content_type_param (ct, "method");
  if (!method)
    return NULL;

  return g_ascii_strup (method, -1);
}

static gboolean
create_calendar (StampMimeParser     *self,
                 StampMimeAttachment *attachment)
{
  ICalComponent *ical = NULL;
  const gchar *ical_text = NULL;
  const gchar *ical_start = NULL;
  gsize len;

  ical_text = g_bytes_get_data (attachment->data, &len);
  if (!ical_text)
    return FALSE;

  ical_start = g_strstr_len (ical_text, len, "BEGIN:VCALENDAR");

  if (!ical_start)
    ical_start = g_strstr_len (ical_text, len, "BEGIN:VCARD");

  if (!ical_start) {
    const gchar *body = NULL;

    body = g_strstr_len (ical_text, len, "\r\n\r\n");
    if (!body)
      body = g_strstr_len (ical_text, len, "\n\n");

    if (body) {
      gsize body_len = len - (body - ical_text);

      body += (body[0] == '\r') ? 4 : 2;
      ical_start = g_strstr_len (body, body_len, "BEGIN:VCALENDAR");
      if (!ical_start)
        ical_start = g_strstr_len (body, body_len, "BEGIN:VCARD");
    }
  }

  if (ical_start) {
    gsize start_len = len - (ical_start - ical_text);
    g_autofree char *tmp = g_strndup (ical_start, start_len);

    ical = e_cal_util_parse_ics_string (tmp);
  } else {
    ical = e_cal_util_parse_ics_string (ical_text);
  }

  if (!ical)
    return FALSE;

  if (self->calendar)
    stamp_mime_calendar_free (self->calendar);

  self->calendar = stamp_mime_calendar_new ();
  self->calendar->ical = ical;
  self->calendar->uid = g_strdup (i_cal_component_get_uid (ical));
  self->calendar->method = g_strdup (attachment->calendar_method);

  return TRUE;
}

static void
handle_attachment (StampMimeParser *self,
                   CamelMimePart   *part,
                   GCancellable    *cancellable)
{
  CamelContentType *content_type = camel_mime_part_get_content_type (part);
  const gchar *type = content_type->type;
  const gchar *subtype = content_type->subtype;
  GByteArray *ba;
  gchar type_lc[32];
  gchar sub_lc[64];
  gsize data_len;
  guint8 *data;
  gsize i = 0;
  const gchar *ptr = type;
  StampMimeAttachment *att;
  const gchar *disp;

  if (!type || !subtype)
    return;

  while (*ptr && i < sizeof (type_lc) - 1)
    type_lc[i++] = g_ascii_tolower (*ptr++);

  type_lc[i] = '\0';
  i = 0;
  ptr = subtype;
  while (*ptr && i < sizeof (sub_lc) - 1)
    sub_lc[i++] = g_ascii_tolower (*ptr++);
  sub_lc[i] = '\0';

  PARSER_LOG ("type_lc %s, sub %s", type_lc, sub_lc);
  if (g_strcmp0 (type_lc, "application") == 0 && (g_strcmp0 (sub_lc, "pgp-encrypted") == 0 || g_strcmp0 (sub_lc, "pgp-signature") == 0 || g_strcmp0 (sub_lc, "pkcs7-signature") == 0 || g_strcmp0 (sub_lc, "x-pkcs7-signature") == 0))
    return;

  ba = decode_part_to_bytes (part, cancellable, NULL);
  if (!ba)
    return;

  data_len = ba->len;
  data = g_byte_array_free (ba, FALSE);

  att = stamp_mime_attachment_new ();
  att->filename = g_strdup (camel_mime_part_get_filename (part));
  att->content_id = g_strdup (camel_mime_part_get_content_id (part));
  att->data = g_bytes_new_take (data, data_len);
  att->size = data_len;
  att->image_format = NULL;
  att->calendar_method = NULL;

  PARSER_LOG ("filename: %s, content_id %s", att->filename, att->content_id);

  disp = camel_mime_part_get_disposition (part);
  PARSER_LOG ("disposition: %s", disp);
  att->is_inline = (disp && g_strcmp0 (disp, "inline") == 0);

  if (g_strcmp0 (type_lc, "application") == 0 && g_strcmp0 (sub_lc, "octet-stream") == 0) {
    att->mime_type = g_strdup ("application/octet-stream");
  } else {
    att->mime_type = camel_content_type_simple (content_type);
  }

  if (g_strcmp0 (type_lc, "image") == 0) {
    att->kind = STAMP_MIME_ATTACHMENT_IMAGE;
    att->image_format = image_format_from_subtype (sub_lc);
    att->content_id = g_strdup (camel_mime_part_get_content_id (part));
    if (att->mime_type && g_strcmp0 (disp ? disp : "", "attachment") != 0)
      att->is_inline = TRUE;
  } else if ((g_strcmp0 (type_lc, "application") == 0 && g_strcmp0 (sub_lc, "ics") == 0) || (g_strcmp0 (type_lc, "text") == 0 && g_strcmp0 (sub_lc, "calendar") == 0)) {
    att->kind = STAMP_MIME_ATTACHMENT_CALENDAR;
    att->calendar_method = calendar_method_from_part (part);
    if (att->calendar_method) {
      att->kind = STAMP_MIME_ATTACHMENT_INVITATION;
      create_calendar (self, att);
    }
    PARSER_LOG ("Calendar %s, internal %d, filename %s, size %" G_GSIZE_FORMAT, att->calendar_method, att->is_inline, att->filename, att->size);
  } else if (g_strcmp0 (type_lc, "application") == 0 && g_strcmp0 (sub_lc, "pgp-keys") == 0) {
    att->kind = STAMP_MIME_ATTACHMENT_PGP_KEY;
    att->is_inline = FALSE;
  } else if (g_strcmp0 (type_lc, "message") == 0 && g_strcmp0 (sub_lc, "rfc822") == 0) {
    att->kind = STAMP_MIME_ATTACHMENT_MESSAGE;
    att->mime_type = g_strdup ("message/rfc822");
  } else {
    att->kind = STAMP_MIME_ATTACHMENT_GENERIC;
  }
  PARSER_LOG ("kind: %x", att->kind);

  self->attachments = g_list_append (self->attachments, att);
}

static gboolean
handle_multipart_alternative (StampMimeParser  *self,
                              CamelMultipart   *mp,
                              GCancellable     *cancellable,
                              GError          **error)
{
  guint num = camel_multipart_get_number (mp);

  for (guint idx = 0; idx < num; idx++) {
    CamelMimePart *child = camel_multipart_get_part (mp, idx);
    g_autoptr (GError) child_err = NULL;

    if (!child)
      continue;

    if (g_cancellable_set_error_if_cancelled (cancellable, error))
      return FALSE;

    if (!dispatch_part (self, child, cancellable, &child_err) && !g_error_matches (child_err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("%s: Error in multipart/alternative part %u: %s", G_STRFUNC, idx, child_err ? child_err->message : "(unknown)");
  }

  return TRUE;
}

static gboolean
handle_multipart_generic (StampMimeParser  *self,
                          CamelMultipart   *mp,
                          GCancellable     *cancellable,
                          GError          **error)
{
  guint num = camel_multipart_get_number (mp);

  for (guint idx = 0; idx < num; idx++) {
    CamelMimePart *child = camel_multipart_get_part (mp, idx);
    g_autoptr (GError) child_err = NULL;

    if (!child)
      continue;

    if (g_cancellable_set_error_if_cancelled (cancellable, error))
      return FALSE;

    if (!dispatch_part (self, child, cancellable, &child_err) && !g_error_matches (child_err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("%s: Error in multipart part %u: %s", G_STRFUNC, idx, child_err ? child_err->message : "(unknown)");
  }

  return TRUE;
}

static gboolean
handle_signed (StampMimeParser  *self,
               CamelMimePart    *part,
               GCancellable     *cancellable,
               GError          **error)
{
  CamelContentType *content_type = camel_mime_part_get_content_type (part);
  const gchar *proto = camel_content_type_param (content_type, "protocol");
  g_autoptr (CamelCipherContext) cipher = NULL;
  gboolean is_smime = FALSE;
  CamelDataWrapper *data_wrapper;

  if (!proto) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "multipart/signed without protocol parameter");
    return FALSE;
  }

  if (g_str_equal (proto, "application/pgp-signature")) {
    cipher = camel_gpg_context_new (self->session);
  } else if (g_str_equal (proto, "application/pkcs7-signature") || g_str_equal (proto, "application/x-pkcs7-signature")) {
    cipher = camel_smime_context_new (self->session);
    is_smime = TRUE;
  } else {
    g_warning ("%s: Unknown signature protocol: %s", G_STRFUNC, proto);
  }

  if (cipher) {
    data_wrapper = camel_medium_get_content (CAMEL_MEDIUM (part));

    if (!CAMEL_IS_MULTIPART_SIGNED (data_wrapper)) {
      g_warning ("%s: Expected CamelMultipartSigned, got %s", G_STRFUNC, G_OBJECT_TYPE_NAME (data_wrapper));
    } else {
      StampMimeSignature *signature;
      g_autoptr (GError) cipher_err = NULL;
      g_autoptr (CamelCipherValidity) validity = NULL;

      signature = g_new0 (StampMimeSignature, 1);
      signature->is_smime = is_smime;
      signature->is_inline = FALSE;

      validity = camel_cipher_context_verify_sync (cipher, CAMEL_MIME_PART (part), cancellable, &cipher_err);
      if (cipher_err) {
        signature->status = STAMP_MIME_SIGNATURE_UNKNOWN;
        signature->description = g_strdup (cipher_err->message);
      } else if (validity) {
        signature->status = map_validity_status (validity->sign.status);
        signature->description = g_strdup (validity->sign.description);
        signature->signers = build_signer_list (validity);
      }

      self->signatures = g_list_append (self->signatures, signature);
    }
  }

  data_wrapper = camel_medium_get_content (CAMEL_MEDIUM (part));
  if (CAMEL_IS_MULTIPART (data_wrapper)) {
    CamelMimePart *content_part = camel_multipart_get_part (CAMEL_MULTIPART (data_wrapper), 0);

    if (content_part)
      return dispatch_part (self, content_part, cancellable, error);
  }

  return TRUE;
}

static gboolean
dispatch_multipart (StampMimeParser  *self,
                    CamelMimePart    *part,
                    gconstpointer     user_data,
                    GCancellable     *cancellable,
                    GError          **error)
{
  CamelDataWrapper *data_wrapper = camel_medium_get_content (CAMEL_MEDIUM (part));

  if (!CAMEL_IS_MULTIPART (data_wrapper))
    return TRUE;

  return GPOINTER_TO_INT (user_data)
      ? handle_multipart_alternative (self, CAMEL_MULTIPART (data_wrapper), cancellable, error)
      : handle_multipart_generic (self, CAMEL_MULTIPART (data_wrapper), cancellable, error);
}

static gboolean
dispatch_text (StampMimeParser  *self,
               CamelMimePart    *part,
               gconstpointer     user_data,
               GCancellable     *cancellable,
               GError          **error)
{
  const gchar *disposition = camel_mime_part_get_disposition (part);

  PARSER_LOG ("disposition %s", disposition ? disposition : "NONE");
  if (g_strcmp0 (disposition, "attachment") == 0) {
    handle_attachment (self, part, cancellable);
    return TRUE;
  }

  handle_text_part (self, part, GPOINTER_TO_INT (user_data), cancellable);

  return TRUE;
}

static gboolean
dispatch_attachment (StampMimeParser  *self,
                     CamelMimePart    *part,
                     gconstpointer     user_data,
                     GCancellable     *cancellable,
                     GError          **error)
{
  handle_attachment (self, part, cancellable);
  return TRUE;
}

static gboolean
dispatch_signed (StampMimeParser  *self,
                 CamelMimePart    *part,
                 gconstpointer     user_data,
                 GCancellable     *cancellable,
                 GError          **error)
{
  return handle_signed (self, part, cancellable, error);
}

static CamelMimePart *
handle_pgp_encrypted_mime (StampMimeParser  *self,
                           CamelMimePart    *part,
                           GCancellable     *cancellable,
                           GError          **error)
{
  g_autoptr (CamelCipherContext) cipher = NULL;
  CamelMimePart *decrypted = NULL;
  CamelDataWrapper *data_wrapper;
  StampMimeEncryption *encryption;

  self->found_encrypted = TRUE;

  encryption = g_new0 (StampMimeEncryption, 1);
  encryption->is_smime = FALSE;
  encryption->is_inline = FALSE;

  cipher = camel_gpg_context_new (self->session);
  data_wrapper = camel_medium_get_content (CAMEL_MEDIUM (part));

  if (!CAMEL_IS_MULTIPART_ENCRYPTED (data_wrapper)) {
    encryption->success = FALSE;
    encryption->status = STAMP_MIME_ENCRYPTION_INVALID;
    encryption->error_message = g_strdup ("No CamelMultipartEncrypted found");
  } else {
    g_autoptr (GError) err = NULL;
    g_autoptr (CamelMimePart) out = camel_mime_part_new ();
    g_autoptr (CamelCipherValidity) cv = camel_cipher_context_decrypt_sync (cipher, CAMEL_MIME_PART (part), out, cancellable, &err);

    if (!cv || err) {
      encryption->success = FALSE;
      encryption->status = STAMP_MIME_ENCRYPTION_INVALID;
      encryption->error_message = err ? g_strdup (err->message) : g_strdup ("PGP MIME Decryption failed");
    } else {
      encryption->success = TRUE;
      encryption->status = STAMP_MIME_ENCRYPTION_VALID;
      decrypted = g_steal_pointer (&out);
    }
  }

  self->encryption = g_list_append (self->encryption, encryption);
  return decrypted;
}

static gboolean
dispatch_pgp_encrypted (StampMimeParser  *self,
                        CamelMimePart    *part,
                        gconstpointer     user_data,
                        GCancellable     *cancellable,
                        GError          **error)
{
  g_autoptr (CamelMimePart) plain = NULL;

  plain = handle_pgp_encrypted_mime (self, part, cancellable, error);
  if (!plain)
    return FALSE;

  return dispatch_part (self, plain, cancellable, error);
}

static CamelMimePart *
handle_smime_encrypted (StampMimeParser  *self,
                        CamelMimePart    *part,
                        GCancellable     *cancellable,
                        GError          **error)
{
  g_autoptr (CamelCipherContext) cipher = NULL;
  g_autoptr (GError) err = NULL;
  g_autoptr (CamelCipherValidity) cv = NULL;
  g_autoptr (CamelMimePart) out = NULL;
  CamelMimePart *decrypted = NULL;
  StampMimeEncryption *encrypt;

  self->found_encrypted = TRUE;

  encrypt = g_new0 (StampMimeEncryption, 1);
  encrypt->is_smime = TRUE;
  encrypt->is_inline = FALSE;

  cipher = camel_smime_context_new (self->session);
  out = camel_mime_part_new ();
  cv = camel_cipher_context_decrypt_sync (cipher, part, out, cancellable, &err);
  if (!cv || err) {
    encrypt->success = FALSE;
    encrypt->status = STAMP_MIME_ENCRYPTION_INVALID;
    encrypt->error_message = err ? g_strdup (err->message) : g_strdup ("S/MIME decryption failed");
  } else {
    encrypt->success = TRUE;
    encrypt->status = STAMP_MIME_ENCRYPTION_VALID;
    decrypted = g_steal_pointer (&out);
  }

  self->encryption = g_list_append (self->encryption, encrypt);
  return decrypted;
}

static gboolean
handle_smime_signed_opaque (StampMimeParser  *self,
                            CamelMimePart    *part,
                            GCancellable     *cancellable,
                            GError          **error)
{
  g_autoptr (CamelCipherContext) cipher = NULL;
  g_autoptr (GError) err = NULL;
  g_autoptr (CamelMimePart) out = NULL;
  g_autoptr (CamelCipherValidity) cv = NULL;
  StampMimeSignature *signature;
  gboolean ok;

  signature = g_new0 (StampMimeSignature, 1);
  signature->is_smime = TRUE;
  signature->is_inline = FALSE;

  cipher = camel_smime_context_new (self->session);
  out = camel_mime_part_new ();
  cv = camel_cipher_context_decrypt_sync (cipher, part, out, cancellable, &err);
  if (err || !cv) {
    signature->status = STAMP_MIME_SIGNATURE_UNKNOWN;
    signature->description = err ? g_strdup (err->message) : g_strdup ("S/MIME opaque signature couldn't be unpacked");

    self->signatures = g_list_append (self->signatures, signature);
    return FALSE;
  }

  ok = dispatch_part (self, out, cancellable, error);

  signature->status = map_validity_status (cv->sign.status);
  signature->description = g_strdup (cv->sign.description);
  signature->signers = build_signer_list (cv);

  self->signatures = g_list_append (self->signatures, signature);

  return ok;
}

static gboolean
dispatch_smime (StampMimeParser  *self,
                CamelMimePart    *part,
                gconstpointer     user_data,
                GCancellable     *cancellable,
                GError          **error)
{
  CamelContentType *content_type = camel_mime_part_get_content_type (part);
  const gchar *smime_type = camel_content_type_param (content_type, "smime-type");

  if (g_strcmp0 (smime_type, "enveloped-data") == 0) {
    g_autoptr (CamelMimePart) plain = NULL;

    plain = handle_smime_encrypted (self, part, cancellable, error);
    if (!plain)
      return FALSE;

    return dispatch_part (self, plain, cancellable, error);
  }

  if (g_strcmp0 (smime_type, "signed-data") == 0)
    return handle_smime_signed_opaque (self, part, cancellable, error);

  handle_attachment (self, part, cancellable);

  return TRUE;
}

static gboolean
dispatch_rfc822 (StampMimeParser  *self,
                 CamelMimePart    *part,
                 gconstpointer     user_data,
                 GCancellable     *cancellable,
                 GError          **error)
{
  CamelDataWrapper *data_wrapper = camel_medium_get_content (CAMEL_MEDIUM (part));

  if (CAMEL_IS_MIME_MESSAGE (data_wrapper)) {
    handle_attachment (self, part, cancellable);

    return dispatch_part (self, CAMEL_MIME_PART (data_wrapper), cancellable, error);
  }

  return TRUE;
}

static const StampPartDispatchEntry part_dispatch_table[] = {
  { "multipart", "signed", dispatch_signed, NULL},
  { "multipart", "encrypted", dispatch_pgp_encrypted, NULL },
  { "multipart", "alternative", dispatch_multipart, GINT_TO_POINTER (1) },
  { "multipart", "*", dispatch_multipart, GINT_TO_POINTER (0) },
  { "application", "pkcs7-mime", dispatch_smime, NULL },
  { "application", "x-pkcs7-mime", dispatch_smime, NULL},
  { "message", "rfc822", dispatch_rfc822, NULL},
  { "text", "html", dispatch_text, GINT_TO_POINTER (1) },
  { "text", "plain", dispatch_text, GINT_TO_POINTER (0) },
  { "text", "*", dispatch_attachment, NULL},
};

static gboolean
dispatch_part (StampMimeParser  *self,
               CamelMimePart    *part,
               GCancellable     *cancellable,
               GError          **error)
{
  CamelContentType *content_type = camel_mime_part_get_content_type (part);
  gboolean ret;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (!content_type)
    return TRUE;

  for (gsize i = 0; i < G_N_ELEMENTS (part_dispatch_table); i++) {
    const StampPartDispatchEntry *entry = &part_dispatch_table[i];

    if (camel_content_type_is (content_type, entry->type, entry->subtype)) {
      self->depth++;
      PARSER_LOG ("%s/%s", entry->type, entry->subtype);
      ret = entry->handler (self, part, entry->user_data, cancellable, error);
      self->depth--;
      return ret;
    }
  }

  self->depth++;
  PARSER_LOG ("%s/%s (attachment fallback)", content_type->type, content_type->subtype);
  ret = dispatch_attachment (self, part, NULL, cancellable, error);
  self->depth--;
  return ret;
}

static void
parse_list_unsubscribe (StampMimeParser  *self,
                        CamelMimeMessage *message)
{
  const gchar *post_header = camel_medium_get_header (CAMEL_MEDIUM (message), "List-Unsubscribe-Post");
  const gchar *header;
  const gchar *ptr;

  if (post_header && strstr (post_header, "One-Click"))
    self->unsubscribe_one_click = TRUE;

  header = camel_medium_get_header (CAMEL_MEDIUM (message), "List-Unsubscribe");
  if (!header)
    return;

  ptr = header;
  while ((ptr = strchr (ptr, '<')) != NULL) {
    StampMimeListUnsubscribe *info;
    g_autofree char *uri = NULL;
    const gchar *end;

    ptr++;

    end = strchr (ptr, '>');
    if (!end)
      break;

    uri = g_strndup (ptr, end - ptr);
    g_strstrip (uri);

    info = stamp_mime_list_unsubscribe_new ();
    info->uri = g_strdup (uri);
    info->one_click = self->unsubscribe_one_click;

    if (g_ascii_strncasecmp (uri, "mailto:", 7) == 0) {
      const gchar *addr_start = uri + 7;
      const gchar *q = strchr (addr_start, '?');

      info->method = STAMP_MIME_UNSUBSCRIBE_MAILTO;

      info->mailto = q ? g_strndup (addr_start, q - addr_start) : g_strdup (addr_start);
    } else if (g_ascii_strncasecmp (uri, "http://", 7) == 0 || g_ascii_strncasecmp (uri, "https://", 8) == 0) {
      info->method = STAMP_MIME_UNSUBSCRIBE_HTTP;
    } else {
      g_clear_pointer (&info, stamp_mime_list_unsubscribe_free);
      ptr = end + 1;
      continue;
    }

    self->list_unsubscribe = g_list_append (self->list_unsubscribe, info);
    ptr = end + 1;
  }
}

gboolean
stamp_mime_parser_parse (StampMimeParser   *self,
                         CamelMimeMessage  *message,
                         GCancellable      *cancellable,
                         GError           **error)
{
  g_return_val_if_fail (STAMP_IS_MIME_PARSER (self), FALSE);
  g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), FALSE);

  PARSER_LOG ("Starting to parse: %s", camel_mime_message_get_subject (message));

  /* Clear internal structure */
  stamp_mime_parser_clear (self);

  /* Parser headers */
  parse_list_unsubscribe (self, message);

  /* Walk tree */
  return dispatch_part (self, CAMEL_MIME_PART (message), cancellable, error);
}

static void
parse_message_thread (GTask        *task,
                      gpointer      source_object,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
  g_autoptr (GError) error = NULL;
  gboolean success;

  if (g_task_return_error_if_cancelled (task))
    return;

  if (cancellable)
    g_cancellable_push_current (cancellable);

  success = stamp_mime_parser_parse (STAMP_MIME_PARSER (source_object), CAMEL_MIME_MESSAGE (task_data), cancellable, &error);

  if (cancellable)
    g_cancellable_pop_current (cancellable);

  if (g_task_return_error_if_cancelled (task))
    return;

  if (error)
    g_task_return_error (task, g_steal_pointer (&error));
  else
    g_task_return_boolean (task, success);
}

void
stamp_mime_parser_parse_async (StampMimeParser     *self,
                               CamelMimeMessage    *message,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (STAMP_IS_MIME_PARSER (self));
  g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, stamp_mime_parser_parse_async);
  g_task_set_task_data (task, g_object_ref (message), g_object_unref);
  g_task_run_in_thread (task, parse_message_thread);
}

gboolean
stamp_mime_parser_parse_finish (StampMimeParser  *self,
                                GAsyncResult     *result,
                                GError          **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, stamp_mime_parser_parse_async), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

StampMimeBody *
stamp_mime_parser_get_body (StampMimeParser *self)
{
  g_return_val_if_fail (STAMP_IS_MIME_PARSER (self), NULL);
  return self->html_body ? self->html_body : self->plain_body;
}

static GList *
stamp_mime_parser_filter_by_kind (StampMimeParser         *self,
                                  StampMimeAttachmentKind  kind,
                                  gboolean                 is_inline)
{
  GList *result = NULL;

  for (GList *l = self->attachments; l; l = l->next) {
    StampMimeAttachment *att = l->data;

    PARSER_LOG ("%p %d %d %s", att, att->kind & kind, att->is_inline, att->filename);
    if (att->kind & kind && att->is_inline == is_inline)
      result = g_list_append (result, att);
  }

  return result;
}

GList *
stamp_mime_parser_get_attachments (StampMimeParser *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  return stamp_mime_parser_filter_by_kind (self, STAMP_MIME_ATTACHMENT_GENERIC | STAMP_MIME_ATTACHMENT_IMAGE | STAMP_MIME_ATTACHMENT_CALENDAR | STAMP_MIME_ATTACHMENT_MESSAGE, FALSE);
}

const GList *
stamp_mime_parser_get_signatures (StampMimeParser *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->signatures;
}

const GList *
stamp_mime_parser_get_encryptions (StampMimeParser *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->encryption;
}

GList *
stamp_mime_parser_get_list_unsubscribe (StampMimeParser *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->list_unsubscribe;
}

static void
send_unsubscribe_callback (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GError) error = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  if (error) {
    g_warning ("%s: Failed to unsubscribe: %s", G_STRFUNC, error->message);
  } else {
    g_debug ("%s: Successfully unsubscribed, size %" G_GSIZE_FORMAT, G_STRFUNC, g_bytes_get_size (bytes));
  }
}

void
stamp_mime_parser_send_unsubscribe (StampMimeParser          *self,
                                    StampMimeListUnsubscribe *unsubscribe,
                                    GCancellable             *cancellable)
{
  g_autoptr (SoupSession) session = NULL;
  g_autoptr (SoupMessage) msg = NULL;

  g_return_if_fail (self != NULL);
  g_return_if_fail (unsubscribe != NULL);
  g_return_if_fail (unsubscribe->uri != NULL);

  session = soup_session_new ();
  msg = soup_message_new ("POST", unsubscribe->uri);

  if (!msg) {
    g_warning ("%s: Could not create SoupMessage for %s", G_STRFUNC, unsubscribe->uri);
    return;
  }

  soup_message_headers_replace (soup_message_get_request_headers (msg), "Content-Type", "application/x-www-form-urlencoded");

  soup_session_send_and_read_async (session,
                                    msg,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    send_unsubscribe_callback,
                                    NULL);
}

GList *
stamp_mime_parser_get_inline_images (StampMimeParser *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  return stamp_mime_parser_filter_by_kind (self, STAMP_MIME_ATTACHMENT_IMAGE, TRUE);
}

StampMimeCalendar *
stamp_mime_parser_get_invitations (StampMimeParser *self)
{
  g_return_val_if_fail (STAMP_IS_MIME_PARSER (self), NULL);

  return self->calendar;
}
