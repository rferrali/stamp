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

#include "config.h"

#include "stamp-composer.h"

#include <camel/camel.h>
#include <glib/gi18n.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libebook-contacts/libebook-contacts.h>
#include <libebook/libebook.h>
#include <libedata-book/libedata-book.h>
#include <libedataserverui4/libedataserverui4.h>

#include "stamp-account.h"
#include "stamp-attachment-button.h"
#include "stamp-composer-from.h"
#include "stamp-contact-completion.h"
#include "stamp-mail-view.h"
#include "stamp-session.h"
#include "stamp-settings.h"
#include "stamp-settings.h"
#include "stamp-signature.h"
#include "stamp-tag.h"
#include "stamp-webview.h"
#include "stamp-window.h"

#define USER_AGENT ("Stamp " PACKAGE_VERSION)

struct _StampComposer {
  AdwApplicationWindow parent_instance;

  GtkWidget *webview_bin;
  StampWebView *webview;
  GtkWidget *from;
  GtkWidget *to;
  GtkWidget *cc;
  GtkWidget *bcc;
  GtkWidget *toggle;
  GtkWidget *send_button;
  GtkWidget *subject;
  GtkWidget *window_title;
  GtkWidget *attachment_revealer;
  GtkWidget *attachment_box;
  GtkWidget *request_disposition;
  GtkWidget *pgp_sign;
  GtkWidget *pgp_encrypt;
  GtkWidget *smime_sign;
  GtkWidget *smime_encrypt;
  GtkWidget *security_menu;
  GtkWidget *security_is_active;
  GtkWidget *security_popover;
  GtkWidget *insert_link_button;
  GtkWidget *scrolled_window;
  StampComposerType type;
  CamelMimeMessage *orig_message;
  gchar *draft_uid;
  GSimpleActionGroup *actions;
  AdwToastOverlay *toast_overlay;

  GtkWidget *signature;
  GSimpleAction *signature_action;
  gboolean has_loaded;

  GList *attachments;
  StampComposerFrom *composer_from;
  gboolean is_dirty;
  StampSignature *active_signature;

  GCancellable *cancellable;
  gboolean discard_draft;
  gint autosave_source_id;
  gboolean ignore_missing_attachments;
};

G_DEFINE_FINAL_TYPE (StampComposer, stamp_composer, ADW_TYPE_APPLICATION_WINDOW);

typedef enum {
  PROP_ACCOUNT = 1,
} StampComposerProps;

static GParamSpec *props[PROP_ACCOUNT + 1] = { NULL, };

typedef struct {
  StampMailView *mail_view;
  StampAccount *account;
  gchar *draft_uid;
} SendCompletedData;

static SendCompletedData *
send_completed_data_new (StampMailView *mail_view,
                         StampAccount  *account,
                         const gchar   *draft_uid)
{
  SendCompletedData *data = g_new0 (SendCompletedData, 1);

  data->mail_view = g_object_ref (mail_view);
  data->account = g_object_ref (account);
  data->draft_uid = g_strdup (draft_uid);

  return data;
}

static void
send_completed_data_free (gpointer data)
{
  SendCompletedData *scd = data;

  g_clear_object (&scd->mail_view);
  g_clear_object (&scd->account);
  g_free (scd->draft_uid);
  g_free (scd);
}

static void
on_query_command (GObject      *source,
                  const gchar  *command,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  StampComposer *self = user_data;
  g_autoptr (GError) error = NULL;
  gboolean ret;

  ret = stamp_web_view_query_command_state_finish (source, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Could not query command state: %s", error->message);
    return;
  }

  g_action_group_change_action_state (G_ACTION_GROUP (self->actions), command, g_variant_new_string (ret ? command : ""));
}

static void
on_query_bold_command (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  on_query_command (source, "bold", res, user_data);
}

static void
on_query_italic_command (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  on_query_command (source, "italic", res, user_data);
}

static void
on_query_underline_command (GObject      *source,
                            GAsyncResult *res,
                            gpointer      user_data)
{
  on_query_command (source, "underline", res, user_data);
}

static void
on_query_strikethrough_command (GObject      *source,
                                GAsyncResult *res,
                                gpointer      user_data)
{
  on_query_command (source, "strikethrough", res, user_data);
}

static void
on_query_bullet_list_command (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  on_query_command (source, "insertUnorderedList", res, user_data);
}

static void
on_query_numbered_list_command (GObject      *source,
                                GAsyncResult *res,
                                gpointer      user_data)
{
  on_query_command (source, "insertOrderedList", res, user_data);
}

static void
update_actions (StampComposer *self)
{
  stamp_web_view_query_command_state (self->webview, "bold", self->cancellable, on_query_bold_command, self);
  stamp_web_view_query_command_state (self->webview, "italic", self->cancellable, on_query_italic_command, self);
  stamp_web_view_query_command_state (self->webview, "underline", self->cancellable, on_query_underline_command, self);
  stamp_web_view_query_command_state (self->webview, "strikethrough", self->cancellable, on_query_strikethrough_command, self);
  stamp_web_view_query_command_state (self->webview, "insertUnorderedList", self->cancellable, on_query_bullet_list_command, self);
  stamp_web_view_query_command_state (self->webview, "insertOrderedList", self->cancellable, on_query_numbered_list_command, self);
}

static void
set_send_action_enabled (StampComposer *self,
                         gboolean       enabled)
{
  GAction *action;

  action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "send");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

static void
on_edit_activate (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  const gchar *command = g_variant_get_string (parameter, NULL);

  webkit_web_view_execute_editing_command (WEBKIT_WEB_VIEW (self->webview), command);
  update_actions (self);
}

static CamelMimeMessage *
build_message (StampComposer *self,
               const gchar   *body_html,
               const gchar   *body_plain)
{
  CamelMimePart *part;
  CamelMultipart *body;
  CamelMimeMessage *message;
  g_autoptr (CamelStream) stream_mem = camel_stream_mem_new_with_buffer (body_html, strlen (body_html));
  g_autoptr (CamelStream) stream_filter = camel_stream_filter_new (stream_mem);
  g_autoptr (CamelDataWrapper) html = camel_data_wrapper_new ();

  body = camel_multipart_new ();
  camel_data_wrapper_set_mime_type (CAMEL_DATA_WRAPPER (body), "multipart/alternative");
  camel_multipart_set_boundary (body, NULL);

  /* Plain text part */
  if (body_plain && *body_plain) {
    g_autoptr (CamelMimePart) plain_text_part = NULL;
    g_autoptr (CamelStream) plain_mem = camel_stream_mem_new_with_buffer (body_plain, strlen (body_plain));
    g_autoptr (CamelStream) plain_filter = camel_stream_filter_new (plain_mem);
    g_autoptr (CamelDataWrapper) plain = camel_data_wrapper_new ();

    camel_data_wrapper_construct_from_stream_sync (plain, plain_filter, self->cancellable, NULL);
    camel_data_wrapper_set_mime_type (plain, "text/plain; charset=utf-8");

    plain_text_part = camel_mime_part_new ();
    camel_medium_set_content (CAMEL_MEDIUM (plain_text_part), CAMEL_DATA_WRAPPER (plain));
    camel_mime_part_set_encoding (plain_text_part, CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE);
    camel_multipart_add_part (body, plain_text_part);
  }

  /* HTML part */
  camel_data_wrapper_construct_from_stream_sync (html, stream_filter, self->cancellable, NULL);
  camel_data_wrapper_set_mime_type (html, "text/html; charset=utf-8");

  part = camel_mime_part_new ();
  camel_medium_set_content (CAMEL_MEDIUM (part), CAMEL_DATA_WRAPPER (html));
  camel_mime_part_set_encoding (part, CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE);
  camel_multipart_add_part (body, part);

  g_clear_object (&part);

  for (GList *attachment = self->attachments; attachment && attachment->data; attachment = g_list_next (attachment)) {
    StampAttachmentButton *button = STAMP_ATTACHMENT_BUTTON (attachment->data);

    part = stamp_attachment_button_get_mime_part (button);
    camel_multipart_add_part (body, part);
    g_clear_object (&part);
  }

  message = camel_mime_message_new ();
  camel_mime_message_set_subject (message, gtk_editable_get_text (GTK_EDITABLE (self->subject)));
  camel_mime_message_set_date (message, CAMEL_MESSAGE_DATE_CURRENT, 0);

  if (self->type == STAMP_COMPOSER_REPLY || self->type == STAMP_COMPOSER_REPLY_ALL) {
    const gchar *msgid = camel_mime_message_get_message_id (self->orig_message);
    const gchar *refs = camel_medium_get_header (CAMEL_MEDIUM (self->orig_message), "References");
    CamelDataWrapper *content;

    content = camel_medium_get_content (CAMEL_MEDIUM (self->orig_message));
    if (CAMEL_IS_MULTIPART (content)) {
      CamelMultipart *mp = CAMEL_MULTIPART (content);
      gint n = camel_multipart_get_number (mp);

      for (gint i = 0; i < n; i++) {
        CamelMimePart *multi_part = camel_multipart_get_part (mp, i);
        CamelContentType *ctype = camel_mime_part_get_content_type (multi_part);

        if (ctype) {
          if (g_strcmp0 (ctype->type, "multipart") == 0 && g_strcmp0 (ctype->subtype, "related") == 0) {
            gint nr;

            nr = camel_multipart_get_number (CAMEL_MULTIPART (camel_medium_get_content (CAMEL_MEDIUM (multi_part))));
            for (gint j = 0; j < nr; j++) {
              CamelMimePart *img = camel_multipart_get_part (CAMEL_MULTIPART (camel_medium_get_content (CAMEL_MEDIUM (multi_part))), j);

              if (camel_mime_part_get_content_id (img))
                camel_multipart_add_part (body, img);
            }
          } else if (g_strcmp0 (ctype->type, "image") == 0) {
            camel_multipart_add_part (body, multi_part);
          }
        }
      }
    }

    if (msgid)
      camel_medium_set_header (CAMEL_MEDIUM (message), "In-Reply-To", msgid);

    if (refs && msgid) {
      g_autofree char *newrefs = g_strdup_printf ("%s %s", refs, msgid);

      camel_medium_set_header (CAMEL_MEDIUM (message), "References", newrefs);
    }
  }

  camel_medium_set_content (CAMEL_MEDIUM (message), CAMEL_DATA_WRAPPER (body));

  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->request_disposition))) {
    const gchar *mail = stamp_composer_from_get_mail (self->composer_from);

    camel_medium_set_header (CAMEL_MEDIUM (message), "Disposition-Notification-To", mail);
  }

  return message;
}

static CamelInternetAddress *
build_sender (CamelMimeMessage *message,
              const gchar      *name,
              const gchar      *mail)
{
  CamelInternetAddress *sender = camel_internet_address_new ();

  camel_internet_address_add (sender, name, mail);
  camel_mime_message_set_from (message, sender);

  return sender;
}

static CamelInternetAddress *
build_recipients (StampComposer    *self,
                  CamelMimeMessage *message)
{
  CamelInternetAddress *recipients = camel_internet_address_new ();
  CamelInternetAddress *cc_recipients;
  GList *tags;

  tags = stamp_contact_completion_get_tags (STAMP_CONTACT_COMPLETION (self->to));
  for (GList *iter = tags; iter && iter->data; iter = g_list_next (iter)) {
    StampTag *tag = STAMP_TAG (iter->data);
    CamelInternetAddress *to_addresses = camel_internet_address_new ();

    camel_internet_address_add (to_addresses, stamp_tag_get_label (tag), stamp_tag_get_mail (tag));
    camel_address_cat (CAMEL_ADDRESS (recipients), CAMEL_ADDRESS (to_addresses));
  }

  camel_mime_message_set_recipients (message, CAMEL_RECIPIENT_TYPE_TO, recipients);

  cc_recipients = camel_internet_address_new ();
  tags = stamp_contact_completion_get_tags (STAMP_CONTACT_COMPLETION (self->cc));
  for (GList *iter = tags; iter && iter->data; iter = g_list_next (iter)) {
    StampTag *tag = STAMP_TAG (iter->data);
    CamelInternetAddress *to_addresses = camel_internet_address_new ();

    camel_internet_address_add (to_addresses, stamp_tag_get_label (tag), stamp_tag_get_mail (tag));
    camel_address_cat (CAMEL_ADDRESS (cc_recipients), CAMEL_ADDRESS (to_addresses));
  }

  if (camel_address_length (CAMEL_ADDRESS (cc_recipients)) > 0) {
    camel_mime_message_set_recipients (message, CAMEL_RECIPIENT_TYPE_CC, cc_recipients);
  }

  return recipients;
}

static void
on_send_mail (GObject      *account,
              GAsyncResult *res,
              gpointer      user_data)
{
  SendCompletedData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!stamp_account_send_mail_finish (STAMP_ACCOUNT (account), res, &error)) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_autofree char *tmp = g_strdup_printf (_("Failed to send mail: %s"), error->message);

      g_warning ("%s", tmp);
      stamp_mail_view_show_toast (data->mail_view, tmp);
    }

    send_completed_data_free (data);
    return;
  }

  if (data->draft_uid)
    stamp_account_remove_draft (data->account, data->draft_uid);

  stamp_mail_view_show_toast (data->mail_view, _("Mail sent"));
  send_completed_data_free (data);
}

static CamelMimeMessage *
wrap_part_in_message (CamelMimePart    *part,
                      CamelMimeMessage *original)
{
  CamelMimeMessage *msg = camel_mime_message_new ();
  CamelDataWrapper *content = camel_medium_get_content (CAMEL_MEDIUM (part));

  if (content)
    camel_medium_set_content (CAMEL_MEDIUM (msg), content);

  if (original) {
    const CamelNameValueArray *hdrs = camel_medium_get_headers (CAMEL_MEDIUM (original));

    for (gint i = 0; i < camel_name_value_array_get_length (hdrs); i++) {
      const gchar *name = camel_name_value_array_get_name (hdrs, i);
      const gchar *value;

      if (!name)
        continue;

      if (g_ascii_strcasecmp (name, "content-type") == 0 ||
          g_ascii_strcasecmp (name, "content-transfer-encoding") == 0 ||
          g_ascii_strcasecmp (name, "mime-version") == 0 ||
          g_ascii_strcasecmp (name, "content-disposition") == 0 ||
          g_ascii_strcasecmp (name, "content-description") == 0 ||
          g_ascii_strcasecmp (name, "content-id") == 0)
        continue;

      value = camel_name_value_array_get_value (hdrs, i);
      if (value)
        camel_medium_set_header (CAMEL_MEDIUM (msg), name, value);
    }
  }

  return msg;
}

static gboolean
apply_crypto (CamelSession          *session,
              CamelMimeMessage     **mime_message,
              CamelInternetAddress  *recipient,
              ESource               *identity,
              const gchar           *account_uid,
              gboolean               pgp_sign,
              gboolean               pgp_encrypt,
              gboolean               smime_sign,
              gboolean               smime_encrypt,
              GCancellable          *cancellable,
              GError               **error)
{
  g_autoptr (GError) local_error = NULL;

  if (pgp_sign || pgp_encrypt) {
    g_autoptr (CamelCipherContext) cipher = camel_gpg_context_new (session);
    g_autoptr (CamelMimePart) opart = camel_mime_part_new ();
    CamelMimePart *ipart;

    ipart = CAMEL_MIME_PART (*mime_message);

    if (pgp_sign) {
      g_autoptr (CamelMimeMessage) old = NULL;

      camel_cipher_context_sign_sync (cipher, NULL, CAMEL_CIPHER_HASH_SHA256, ipart, opart, cancellable, &local_error);
      if (local_error) {
        g_set_error (error, G_IO_ERROR, 0, "PGP signing failed: %s", local_error->message);
        return FALSE;
      }

      old = *mime_message;
      if (CAMEL_IS_MIME_MESSAGE (opart))
        *mime_message = CAMEL_MIME_MESSAGE (g_object_ref (opart));
      else
        *mime_message = wrap_part_in_message (opart, old);

      ipart = CAMEL_MIME_PART (*mime_message);
      opart = camel_mime_part_new ();
    }

    if (pgp_encrypt) {
      g_autoptr (CamelMimeMessage) old = NULL;
      g_autoptr (GPtrArray) recipients_list = g_ptr_array_new ();

      for (gint i = 0; i < camel_address_length (CAMEL_ADDRESS (recipient)); i++) {
        const gchar *r_name, *r_mail;
        if (camel_internet_address_get (recipient, i, &r_name, &r_mail) && r_mail) {
          g_ptr_array_add (recipients_list, g_strdup (r_mail));
        }
      }

      camel_cipher_context_encrypt_sync (cipher, NULL, recipients_list, ipart, opart, cancellable, &local_error);
      if (local_error) {
        g_set_error (error, G_IO_ERROR, 0, "PGP encryption failed: %s", local_error->message);
        return FALSE;
      }

      old = *mime_message;
      if (CAMEL_IS_MIME_MESSAGE (opart))
        *mime_message = CAMEL_MIME_MESSAGE (g_object_ref (opart));
      else
        *mime_message = wrap_part_in_message (opart, old);
    }

    return TRUE;
  }

  if (smime_sign || smime_encrypt) {
    g_autofree char *account_settings_path = g_strdup_printf ("/org/tabos/stamp/mail/accounts/%s/", account_uid);
    g_autoptr (GSettings) account_settings = g_settings_new_with_path ("org.tabos.stamp.mail.accounts", account_settings_path);
    g_autofree char *sign_cert = g_settings_get_string (account_settings, "smime-sign-cert");
    g_autofree char *encrypt_cert = g_settings_get_string (account_settings, "smime-encrypt-cert");
    g_autoptr (CamelCipherContext) cipher = camel_smime_context_new (session);
    g_autoptr (CamelMimePart) opart = camel_mime_part_new ();
    CamelMimePart *ipart = CAMEL_MIME_PART (*mime_message);

    if (smime_sign) {
      g_autoptr (CamelMimeMessage) old = NULL;

      if (!sign_cert) {
        g_set_error (error, G_IO_ERROR, 0, "S/MIME signing skipped: no signing certificate configured");
        return FALSE;
      }

      camel_cipher_context_sign_sync (cipher, sign_cert, CAMEL_CIPHER_HASH_SHA256, ipart, opart, cancellable, &local_error);
      if (local_error) {
        g_set_error (error, G_IO_ERROR, 0, "S/MIME signing failed: %s", local_error->message);
        return FALSE;
      }

      old = *mime_message;
      if (CAMEL_IS_MIME_MESSAGE (opart))
        *mime_message = CAMEL_MIME_MESSAGE (g_object_ref (opart));
      else
        *mime_message = wrap_part_in_message (opart, old);

      ipart = CAMEL_MIME_PART (*mime_message);
      opart = camel_mime_part_new ();
    }

    if (smime_encrypt) {
      g_autoptr (GPtrArray) recipients_list = NULL;
      g_autoptr (CamelMimeMessage) old = NULL;

      if (!encrypt_cert) {
        g_set_error (error, G_IO_ERROR, 0, "S/MIME encryption skipped: no encryption certificate configured");
        return FALSE;
      }

      recipients_list = g_ptr_array_new ();
      for (gint i = 0; i < camel_address_length (CAMEL_ADDRESS (recipient)); i++) {
        const gchar *r_name, *r_mail;

        if (camel_internet_address_get (recipient, i, &r_name, &r_mail) && r_mail) {
          g_ptr_array_add (recipients_list, g_strdup (r_mail));
        }
      }

      camel_cipher_context_encrypt_sync (cipher, encrypt_cert, recipients_list, ipart, opart, cancellable, &local_error);
      if (local_error) {
        g_set_error (error, G_IO_ERROR, 0, "S/MIME encryption failed: %s", local_error->message);
        return FALSE;
      }

      old = *mime_message;
      if (CAMEL_IS_MIME_MESSAGE (opart))
        *mime_message = CAMEL_MIME_MESSAGE (g_object_ref (opart));
      else
        *mime_message = wrap_part_in_message (opart, old);
    }
  }

  return TRUE;
}

static void on_get_body_html (GObject      *source_object,
                              GAsyncResult *res,
                              gpointer      user_data);

static gboolean
check_attachment_reminder (StampComposer *self,
                           const gchar   *body)
{
  const gchar *keywords[] = {
    "attachment",
    "anhang",
    "attached",
    "beigefügt",
    "anbei",
    "file attached",
    "siehe anhang",
    "see attachment",
    "see attached",
    NULL
  };

  if (self->attachments != NULL)
    return FALSE;

  for (gint i = 0; keywords[i] != NULL; i++) {
    g_autofree char *body_lower = g_ascii_strdown (body, -1);

    if (g_strrstr (body_lower, keywords[i]))
      return TRUE;
  }

  return FALSE;
}

static void
on_attachment_reminder_response (GtkWidget *dialog,
                                 gchar     *response,
                                 gpointer   user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  if (g_strcmp0 (response, "send-anyway") == 0) {
    self->ignore_missing_attachments = TRUE;
    stamp_webview_get_body_html (self->webview, NULL, on_get_body_html, self);
  } else {
    set_send_action_enabled (self, TRUE);
  }
}

static void
show_attachment_reminder (StampComposer *self)
{
  AdwDialog *dialog;

  dialog = adw_alert_dialog_new (_("Missing Attachment"), _("Your message mentions an attachment, but none has been added. Do you want to send anyway?"));

  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("_Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "send-anyway", _("Send Anyway"));

  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "send-anyway", ADW_RESPONSE_DESTRUCTIVE);

  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

  g_signal_connect (dialog, "response", G_CALLBACK (on_attachment_reminder_response), self);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_get_body_html (GObject      *source_object,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  StampComposer *self = user_data;
  StampWebView *web_view = STAMP_WEB_VIEW (source_object);
  StampMailView *mail_view = NULL;
  StampAccount *account;
  g_autoptr (GError) error = NULL;
  g_autofree char *body_plain = NULL;
  g_autofree char *body = stamp_webview_get_body_html_finish (web_view, res, &body_plain, &error);
  CamelMimeMessage *mime_message;
  CamelInternetAddress *sender;
  CamelInternetAddress *recipient;
  SendCompletedData *data;
  const gchar *name;
  const gchar *mail;
  gboolean do_pgp_sign;
  gboolean do_pgp_encrypt;
  gboolean do_smime_sign;
  gboolean do_smime_encrypt;
  g_autofree gchar *message_uid = NULL;
  const gchar *from_domain;
  const gchar *at;

  if (!body) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Failed to get HTML content for mail: %s", error->message);
    return;
  }

  if (!self->ignore_missing_attachments && check_attachment_reminder (self, body)) {
    show_attachment_reminder (self);
    return;
  }

  mime_message = build_message (self, body, body_plain);

  name = stamp_composer_from_get_name (self->composer_from);
  mail = stamp_composer_from_get_mail (self->composer_from);
  account = stamp_composer_from_get_account (self->composer_from);

  sender = build_sender (mime_message, name, mail);
  recipient = build_recipients (self, mime_message);

  do_pgp_sign = gtk_check_button_get_active (GTK_CHECK_BUTTON (self->pgp_sign));
  do_pgp_encrypt = gtk_check_button_get_active (GTK_CHECK_BUTTON (self->pgp_encrypt));
  do_smime_sign = gtk_check_button_get_active (GTK_CHECK_BUTTON (self->smime_sign));
  do_smime_encrypt = gtk_check_button_get_active (GTK_CHECK_BUTTON (self->smime_encrypt));

  if (do_pgp_sign || do_pgp_encrypt || do_smime_sign || do_smime_encrypt) {
    CamelSession *session;
    ESource *identity = NULL;

    session = CAMEL_SESSION (stamp_session_get_default ());
    if (account) {
      StampMailService *mail_service = stamp_account_get_mail_service (account);

      if (mail_service)
        identity = stamp_mail_service_get_identity_source (mail_service);
    }

    apply_crypto (session,
                  &mime_message,
                  recipient,
                  identity,
                  stamp_account_get_uid (account),
                  do_pgp_sign,
                  do_pgp_encrypt,
                  do_smime_sign,
                  do_smime_encrypt,
                  self->cancellable,
                  &error);
    if (error) {
      AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Cannot send mail"), error->message));

      adw_alert_dialog_add_response (dialog, "close", _("_Close"));
      adw_alert_dialog_set_default_response (dialog, "close");
      adw_alert_dialog_set_close_response (dialog, "close");
      adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
      return;
    }
  }

  mail_view = stamp_window_get_mail_view (STAMP_WINDOW (stamp_get_main_window ()));

  data = send_completed_data_new (mail_view, account, self->draft_uid);

  camel_medium_set_header (CAMEL_MEDIUM (mime_message), "User-Agent", USER_AGENT);

  at = strchr (mail, '@');
  if (at)
    from_domain = at + 1;
  else
    from_domain = NULL;

  if (!from_domain || !*from_domain)
    from_domain = "localhost";

  message_uid = camel_header_msgid_generate (from_domain);
  camel_mime_message_set_message_id (mime_message, message_uid);

  stamp_account_send_mail (account, mime_message, sender, recipient,
                           do_pgp_sign || do_smime_sign, do_pgp_encrypt || do_smime_encrypt, NULL, on_send_mail, data);

  self->is_dirty = FALSE;
  stamp_mail_view_show_toast (mail_view, _("Sending Mail…"));
  gtk_window_destroy (GTK_WINDOW (self));
}

static void
stamp_composer_set_composer_from (StampComposer     *self,
                                  StampComposerFrom *from)
{
  GListModel *model = gtk_drop_down_get_model (GTK_DROP_DOWN (self->from));

  if (!model)
    return;

  for (guint idx = 0; idx < g_list_model_get_n_items (model); idx++) {
    StampComposerFrom *cur_from = STAMP_COMPOSER_FROM (g_list_model_get_item (model, idx));

    if (from == cur_from) {
      StampAccount *account = stamp_composer_from_get_account (from);

      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->from), idx);
      self->composer_from = from;

      stamp_contact_completion_set_account (STAMP_CONTACT_COMPLETION (self->to), account);
      stamp_contact_completion_set_account (STAMP_CONTACT_COMPLETION (self->cc), account);
      stamp_contact_completion_set_account (STAMP_CONTACT_COMPLETION (self->bcc), account);
      return;
    }
  }
}

static void
stamp_composer_set_composer_from_by_account (StampComposer *self,
                                             StampAccount  *account)
{
  GListModel *model = gtk_drop_down_get_model (GTK_DROP_DOWN (self->from));

  if (!model)
    return;

  for (guint idx = 0; idx < g_list_model_get_n_items (model); idx++) {
    StampComposerFrom *cur_from = STAMP_COMPOSER_FROM (g_list_model_get_item (model, idx));
    StampAccount *cur_account = stamp_composer_from_get_account (cur_from);

    if (account == cur_account) {
      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->from), idx);
      self->composer_from = cur_from;

      stamp_contact_completion_set_account (STAMP_CONTACT_COMPLETION (self->to), account);
      stamp_contact_completion_set_account (STAMP_CONTACT_COMPLETION (self->cc), account);
      stamp_contact_completion_set_account (STAMP_CONTACT_COMPLETION (self->bcc), account);
      return;
    }
  }
}

static void
on_send_activated (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  set_send_action_enabled (self, FALSE);

  if (strlen (gtk_editable_get_text (GTK_EDITABLE (self->subject))) == 0) {
    AdwDialog *dialog = adw_alert_dialog_new ("Send without subject?", "This message has an empty subject field. The recipient may be unable to infer its scope or importance.");

    adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                    "cancel", ("_Cancel"),
                                    "send-anyway", ("Send Anyway"),
                                    NULL);

    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
                                              "send-anyway",
                                              ADW_RESPONSE_DESTRUCTIVE);

    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    adw_dialog_present (dialog, GTK_WIDGET (self));
  } else {
    stamp_webview_get_body_html (self->webview, NULL, on_get_body_html, self);
  }
}

static void
on_remove_format_activated (GSimpleAction *action,
                            GVariant      *parameter,
                            gpointer       user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  stamp_web_view_execute_editor_command (self->webview, "removeformat", "");
  stamp_web_view_execute_editor_command (self->webview, "unlink", "");
}

static void
on_insert_signature_finished (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (source), res, &error);
  if (error)
    g_warning ("insert_signature: JS error: %s", error->message);
}

static void
insert_signature (StampComposer  *self,
                  StampSignature *sig)
{
  const gchar *content = stamp_signature_get_content (sig);
  g_autofree char *encoded = g_base64_encode ((const guchar *)content, strlen (content));
  g_autofree char *js = g_strdup_printf (
    "(function(){"
    "  var bytes = Uint8Array.from(atob('%s'), function(c){return c.charCodeAt(0)}), html = new TextDecoder('utf-8').decode(bytes), el = document.getElementById('mail-signature');"
    "  var ed = document.getElementById('message-body');"
    "  if (!ed) return 'no-editor';"
    "  if (el) { el.outerHTML = '<div id=\"mail-signature\">' + html + '</div>'; return 'replaced'; }"
    "  ed.appendChild(document.createElement('br'));"
    "  ed.focus();"
    "  var div = document.createElement('div');"
    "  div.id = 'mail-signature';"
    "  div.innerHTML = html;"
    "  ed.appendChild(div);"
    "  return 'appended';"
    "})()",
    encoded);

  gtk_widget_grab_focus (GTK_WIDGET (self->webview));
  webkit_web_view_evaluate_javascript (WEBKIT_WEB_VIEW (self->webview), js, -1, NULL, NULL, NULL, (GAsyncReadyCallback)on_insert_signature_finished, self);
  self->active_signature = sig;
}

static void
on_signature_activated (GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  const gchar *target = g_variant_get_string (parameter, NULL);
  StampSignature *new_sig = NULL;

  if (target && target[0]) {
    GList *sigs = stamp_session_get_signatures (stamp_session_get_default ());

    for (GList *iter = sigs; iter && iter->data; iter = g_list_next (iter)) {
      StampSignature *s = (StampSignature *)iter->data;

      if (g_strcmp0 (stamp_signature_get_name (s), target) == 0) {
        new_sig = s;
        break;
      }
    }
  }

  if (new_sig == self->active_signature)
    return;

  if (self->active_signature)
    webkit_web_view_evaluate_javascript (WEBKIT_WEB_VIEW (self->webview),
                                         "(function(){ var el = document.getElementById('mail-signature'); if (el) { el.remove(); return 'removed'; } return 'not-found'; })()",
                                         -1, NULL, NULL, NULL, NULL, NULL);

  self->active_signature = NULL;

  if (new_sig)
    insert_signature (self, new_sig);

  g_simple_action_set_state (action, parameter);
}

#define LARGE_IMAGE_THRESHOLD (300 * 1024)

static void
attach_box_update_max_height (GtkScrolledWindow *sw,
                              AdwWrapBox        *wrap)
{
  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (wrap));
  gint nat = 0;

  if (child == NULL)
    return;

  gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, -1, NULL, &nat, NULL, NULL);

  gtk_scrolled_window_set_max_content_height (sw, 2.5 * nat + adw_wrap_box_get_line_spacing (wrap));
}

typedef struct {
  StampComposer *self;
  gint total;
  gint current;
  GFile **files;
} AttachmentBatch;

typedef struct {
  AttachmentBatch *batch;
  GFile *file;
  gchar *filename;
  gchar *content_type;
  gsize original_size;

  GBytes *small_data;
  gint small_w;
  gint small_h;
  gsize small_size;

  GBytes *medium_data;
  gint medium_w;
  gint medium_h;
  gsize medium_size;

  GBytes *large_data;
  gint large_w;
  gint large_h;
  gsize large_size;
} ResizeData;

static void
resize_data_free (ResizeData *data)
{
  g_clear_pointer (&data->small_data, g_bytes_unref);
  g_clear_pointer (&data->medium_data, g_bytes_unref);
  g_clear_pointer (&data->large_data, g_bytes_unref);
  g_clear_object (&data->file);
  g_free (data->filename);
  g_free (data->content_type);
  g_free (data);
}

static void
attachment_batch_free (AttachmentBatch *batch)
{
  if (batch->files) {
    for (gint i = 0; i < batch->total; i++)
      g_clear_object (&batch->files[i]);

    g_free (batch->files);
  }

  g_free (batch);
}

static void
process_attachment_file (AttachmentBatch *batch);

static void
finish_attachments (AttachmentBatch *batch)
{
  attach_box_update_max_height (GTK_SCROLLED_WINDOW (batch->self->scrolled_window), ADW_WRAP_BOX (batch->self->attachment_box));
  gtk_revealer_set_reveal_child (GTK_REVEALER (batch->self->attachment_revealer), TRUE);
  attachment_batch_free (batch);
}

static void
attachment_add_simple (AttachmentBatch *batch,
                       GFile           *file)
{
  StampComposer *self = batch->self;
  GtkWidget *button = stamp_attachment_button_new_from_file (file);

  self->attachments = g_list_append (self->attachments, button);
  adw_wrap_box_append (ADW_WRAP_BOX (self->attachment_box), button);

  batch->current++;

  if (batch->current >= batch->total)
    finish_attachments (batch);
  else
    process_attachment_file (batch);
}

static void
on_image_resize_response (AdwAlertDialog *dialog,
                          gchar          *response,
                          ResizeData     *data)
{
  AttachmentBatch *batch = data->batch;
  StampComposer *self = batch->self;

  if (g_strcmp0 (response, "cancel") != 0) {
    GtkWidget *button = NULL;

    if (g_strcmp0 (response, "large") == 0 && data->large_data) {
      button = stamp_attachment_button_new_from_data (data->filename, data->content_type, data->large_size, data->large_data, TRUE);
    } else if (g_strcmp0 (response, "medium") == 0 && data->medium_data) {
      button = stamp_attachment_button_new_from_data (data->filename, data->content_type, data->medium_size, data->medium_data, TRUE);
    } else if (g_strcmp0 (response, "small") == 0 && data->small_data) {
      button = stamp_attachment_button_new_from_data (data->filename, data->content_type, data->small_size, data->small_data, TRUE);
    } else {
      button = stamp_attachment_button_new_from_file (data->file);
    }

    self->attachments = g_list_append (self->attachments, button);
    adw_wrap_box_append (ADW_WRAP_BOX (self->attachment_box), button);
  }

  batch->current++;

  if (batch->current >= batch->total)
    finish_attachments (batch);
  else
    process_attachment_file (batch);
}

static void
compute_resize_sizes (GdkPixbuf  *pixbuf,
                      ResizeData *data)
{
  gint og_w = gdk_pixbuf_get_width (pixbuf);
  gint og_h = gdk_pixbuf_get_height (pixbuf);
  gint sizes[3] = { og_w * 0.25, og_w * 0.5, og_w * 0.75 };
  gint *out_ws[3] = { &data->small_w, &data->medium_w, &data->large_w };
  gint *out_hs[3] = { &data->small_h, &data->medium_h, &data->large_h };
  GBytes **out_datas[3] = { &data->small_data, &data->medium_data, &data->large_data };
  gsize *out_sizes[3] = { &data->small_size, &data->medium_size, &data->large_size };

  for (gint i = 0; i < 3; i++) {
    gint max_dim = sizes[i];
    gint new_w, new_h;
    g_autoptr (GdkPixbuf) scaled = NULL;
    gchar *buf = NULL;
    gsize len = 0;
    g_autoptr (GError) error = NULL;

    if (og_w <= max_dim && og_h <= max_dim) {
      new_w = og_w;
      new_h = og_h;
      scaled = g_object_ref (pixbuf);
    } else {
      gdouble scale = (og_w > og_h) ? (gdouble)max_dim / og_w : (gdouble)max_dim / og_h;

      new_w = (gint)(og_w * scale);
      new_h = (gint)(og_h * scale);
      scaled = gdk_pixbuf_scale_simple (pixbuf, new_w, new_h, GDK_INTERP_BILINEAR);
    }

    *out_ws[i] = new_w;
    *out_hs[i] = new_h;

    gdk_pixbuf_save_to_buffer (scaled, &buf, &len, "png", &error, NULL);
    if (!error) {
      *out_datas[i] = g_bytes_new_take (buf, len);
      *out_sizes[i] = len;
    } else {
      g_warning ("%s: Could not save buffer: %s", G_STRFUNC, error->message);
    }
  }
}

static void
present_image_resize_dialog (AttachmentBatch *batch,
                             GFile           *file,
                             GdkPixbuf       *pixbuf,
                             GFileInfo       *info,
                             const gchar     *content_type,
                             gsize            file_size)
{
  StampComposer *self = batch->self;
  const gchar *mime_type = content_type ? g_content_type_get_mime_type (content_type) : NULL;
  ResizeData *data = g_new0 (ResizeData, 1);
  g_autofree gchar *heading = NULL;
  AdwDialog *dialog;
  g_autofree gchar *body = NULL;
  g_autofree gchar *actual_label = NULL;

  data->batch = batch;
  data->file = g_object_ref (file);
  data->filename = g_strdup (g_file_info_get_display_name (info));
  data->content_type = g_strdup (mime_type ? mime_type : "image/jpeg");
  data->original_size = file_size;

  heading = g_strdup_printf (_("Large image: %s"), data->filename);
  body = g_strdup_printf (_("The image is %s. Choose a size:"), g_format_size (file_size));
  dialog = ADW_DIALOG (adw_alert_dialog_new (heading, body));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("_Cancel"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "cancel", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

  actual_label = g_strdup_printf ("Actual Size (%s)", g_format_size (file_size));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "actual", actual_label);

  if (pixbuf) {
    g_autofree gchar *large_label = NULL;
    g_autofree gchar *medium_label = NULL;
    g_autofree gchar *small_label = NULL;

    compute_resize_sizes (pixbuf, data);

    large_label = g_strdup_printf ("Large (%s)", g_format_size (data->large_size));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "large", large_label);

    medium_label = g_strdup_printf ("Medium (%s)", g_format_size (data->medium_size));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "medium", medium_label);

    small_label = g_strdup_printf ("Small (%s)", g_format_size (data->small_size));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "small", small_label);

    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "medium");
  } else {
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "actual");
  }

  g_signal_connect_data (dialog, "response", G_CALLBACK (on_image_resize_response), data, (GClosureNotify)resize_data_free, 0);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
process_attachment_file (AttachmentBatch *batch)
{
  GFile *file = batch->files[batch->current];
  g_autoptr (GFileInfo) info = NULL;
  g_autoptr (GdkPixbuf) pixbuf = NULL;
  const gchar *content_type = NULL;
  const gchar *mime_type = NULL;
  gsize file_size = 0;
  g_autofree gchar *path = NULL;

  info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (!info) {
    attachment_add_simple (batch, file);
    return;
  }

  content_type = g_file_info_get_content_type (info);
  file_size = g_file_info_get_size (info);
  mime_type = content_type ? g_content_type_get_mime_type (content_type) : NULL;

  if (!mime_type || !g_str_has_prefix (mime_type, "image/") || file_size <= LARGE_IMAGE_THRESHOLD) {
    attachment_add_simple (batch, file);
    return;
  }

  path = g_file_get_path (file);
  if (path) {
    g_autoptr (GError) error = NULL;

    pixbuf = gdk_pixbuf_new_from_file (path, &error);
    if (error)
      g_warning ("%s: Could not load pixbuf: %s", G_STRFUNC, error->message);
  }

  present_image_resize_dialog (batch, file, pixbuf, info, content_type, file_size);
}

static void
on_attachment_added (GObject      *obj,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  GtkFileDialog *dialog = GTK_FILE_DIALOG (obj);
  g_autoptr (GError) error = NULL;
  g_autoptr (GListModel) model = NULL;
  gint total;
  gint count = 0;
  GFile **files;
  AttachmentBatch *batch;

  model = gtk_file_dialog_open_multiple_finish (dialog, res, &error);
  if (error) {
    g_warning ("%s: Could not get attachment file(s): %s", G_STRFUNC, error->message);
    return;
  }

  total = g_list_model_get_n_items (model);
  if (total == 0)
    return;

  files = g_new0 (GFile *, total);

  for (gint idx = 0; idx < total; idx++) {
    GFile *file = g_list_model_get_item (model, idx);

    if (file)
      files[count++] = file;
  }

  batch = g_new0 (AttachmentBatch, 1);
  batch->self = self;
  batch->total = count;
  batch->current = 0;
  batch->files = files;

  process_attachment_file (batch);
}

static void
on_add_attachment_activated (GSimpleAction *action,
                             GVariant      *parameter,
                             gpointer       user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_open_multiple (dialog, GTK_WINDOW (self), self->cancellable, on_attachment_added, self);
}

static void
load_from_combobox (StampComposer *self)
{
  GListStore *store;
  GList *accounts;

  accounts = stamp_session_get_accounts (stamp_session_get_default ());

  store = g_list_store_new (STAMP_TYPE_COMPOSER_FROM);

  for (GList *iter = accounts; iter && iter->data; iter = g_list_next (iter)) {
    StampAccount *account = STAMP_ACCOUNT (iter->data);
    StampMailService *service = stamp_account_get_mail_service (account);
    CamelInternetAddress *addresses = stamp_account_get_address (account);
    StampComposerFrom *from;
    GHashTable *aliases;
    const gchar *name;
    const gchar *mail;

    if (!service || !stamp_mail_service_get_enabled (service))
      continue;

    aliases = stamp_mail_service_get_aliases (service);
    if (aliases) {
      GList *mails = g_hash_table_get_keys (aliases);

      for (GList *mail_iter = mails; mail_iter && mail_iter->data; mail_iter = g_list_next (mail_iter)) {
        mail = mail_iter->data;
        name = g_hash_table_lookup (aliases, mail);

        from = stamp_composer_from_new (account, name, mail);
        g_list_store_append (store, from);

        if (!self->composer_from)
          stamp_composer_set_composer_from (self, from);
      }
      g_list_free (mails);
    } else {
      camel_internet_address_get (addresses, 0, &name, &mail);
      from = stamp_composer_from_new (account, name, mail);

      g_list_store_append (store, from);
      if (!self->composer_from)
        stamp_composer_set_composer_from (self, from);
    }
  }


  gtk_drop_down_set_model (GTK_DROP_DOWN (self->from), G_LIST_MODEL (store));
}

static void
on_from_setup (GtkSignalListItemFactory *f,
               GtkListItem              *item,
               gpointer                  data)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *name;
  GtkWidget *email;

  gtk_widget_set_halign (box, GTK_ALIGN_START);
  gtk_widget_set_hexpand (box, TRUE);
  gtk_widget_set_size_request (box, 300, -1);

  name = gtk_label_new (NULL);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_widget_set_halign (name, GTK_ALIGN_START);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (box), name);
  g_object_set_data (G_OBJECT (box), "name", name);

  email = gtk_label_new (NULL);
  gtk_widget_set_hexpand (email, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (email), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign (email, GTK_ALIGN_START);
  gtk_widget_add_css_class (email, "dim-label");
  gtk_widget_add_css_class (email, "smaller");
  gtk_box_append (GTK_BOX (box), email);
  g_object_set_data (G_OBJECT (box), "email", email);

  gtk_list_item_set_child (item, box);
}

static void
on_from_bind (GtkSignalListItemFactory *f,
              GtkListItem              *item,
              gpointer                  user_data)
{
  StampComposerFrom *from = STAMP_COMPOSER_FROM (gtk_list_item_get_item (item));
  GtkWidget *box = gtk_list_item_get_child (item);
  GtkWidget *name = g_object_get_data (G_OBJECT (box), "name");
  GtkWidget *email = g_object_get_data (G_OBJECT (box), "email");
  const gchar *name_str = stamp_composer_from_get_name (from);
  const gchar *mail_str = stamp_composer_from_get_mail (from);

  gtk_label_set_text (GTK_LABEL (name), name_str ? name_str : mail_str);
  gtk_label_set_text (GTK_LABEL (email), mail_str);
}

static void
on_setup_selected (GtkSignalListItemFactory *f,
                   GtkListItem              *item,
                   gpointer                  data)
{
  GtkWidget *name;

  name = gtk_label_new (NULL);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_list_item_set_child (item, name);
}

static void
on_bind_selected (GtkSignalListItemFactory *f,
                  GtkListItem              *item,
                  gpointer                  user_data)
{
  StampComposerFrom *from = STAMP_COMPOSER_FROM (gtk_list_item_get_item (item));
  GtkWidget *name = gtk_list_item_get_child (item);
  const gchar *name_str = stamp_composer_from_get_name (from);
  const gchar *mail_str = stamp_composer_from_get_mail (from);
  g_autofree gchar *markup = g_strdup_printf ("<b>%s</b> (%s)", name_str, g_markup_escape_text (mail_str, -1));

  gtk_label_set_markup (GTK_LABEL (name), markup);
  gtk_widget_set_tooltip_text (name, mail_str);
}

static void
mark_dirty (StampComposer *self);

static void
on_subject_changed (GtkWidget *entry,
                    gpointer   user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (entry));

  if (text && strlen (text) > 0) {
    adw_window_title_set_title (ADW_WINDOW_TITLE (self->window_title), text);
  } else {
    adw_window_title_set_title (ADW_WINDOW_TITLE (self->window_title), _("New Message"));
  }

  mark_dirty (self);
}

static void
on_from_selected_item (GObject    *source,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  StampComposerFrom *from = STAMP_COMPOSER_FROM (gtk_drop_down_get_selected_item (GTK_DROP_DOWN (self->from)));

  if (!from)
    return;

  stamp_composer_set_composer_from (self, from);
}

static void
on_auto_save_get_body_html (GObject      *source_object,
                            GAsyncResult *res,
                            gpointer      user_data)
{
  StampComposer *self = user_data;
  StampWebView *web_view = STAMP_WEB_VIEW (source_object);
  g_autoptr (GError) error = NULL;
  g_autofree char *body_plain = NULL;
  g_autofree char *body = stamp_webview_get_body_html_finish (web_view, res, &body_plain, &error);
  CamelMimeMessage *mime_message;
  CamelInternetAddress *sender;
  CamelInternetAddress *recipient;
  const gchar *name;
  const gchar *mail;

  if (!body) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Failed to get HTML content for mail: %s", error->message);
    return;
  }

  mime_message = build_message (self, body, body_plain);

  name = stamp_composer_from_get_name (self->composer_from);
  mail = stamp_composer_from_get_mail (self->composer_from);

  sender = build_sender (mime_message, name, mail);
  recipient = build_recipients (self, mime_message);

  self->draft_uid = stamp_account_save_draft (stamp_composer_from_get_account (self->composer_from), self->draft_uid, mime_message, sender, recipient);
  if (self->draft_uid) {
    g_autofree char *save_str = NULL;
    GDateTime *dt = g_date_time_new_now_local ();
    g_autofree char *time = g_date_time_format (dt, "%H:%M");

    save_str = g_strdup_printf ("Saved Draft (%s)", time);
    adw_window_title_set_subtitle (ADW_WINDOW_TITLE (self->window_title), save_str);
  }
}

static void
autosave_timeout_cb (gpointer user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  self->autosave_source_id = 0;
  stamp_webview_get_body_html (self->webview, self->cancellable, on_auto_save_get_body_html, self);
}

static void
mark_dirty (StampComposer *self)
{
  self->is_dirty = TRUE;

  if (self->autosave_source_id)
    g_source_remove (self->autosave_source_id);

  self->autosave_source_id = g_timeout_add_seconds_once (10, autosave_timeout_cb, self);
}

static void
on_dirty_message (WebKitUserContentManager *manager,
                  WebKitUserMessage        *message,
                  gpointer                  user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  mark_dirty (self);
}

static void
on_load_changed (WebKitWebView   *view,
                 WebKitLoadEvent  load_event,
                 gpointer         user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  g_autoptr (GBytes) js = NULL;

  if (load_event != WEBKIT_LOAD_FINISHED)
    return;

  webkit_web_view_evaluate_javascript (view, "document.body.addEventListener('input', () => {"
                                       "    window.webkit.messageHandlers.dirty.postMessage('');"
                                       "});",
                                       -1, NULL, NULL, NULL, NULL, NULL);

  js = g_resources_lookup_data ("/org/tabos/stamp/stamp-composer.js", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  webkit_web_view_evaluate_javascript (view,
                                       g_bytes_get_data (js, NULL),
                                       -1, NULL, NULL, NULL, NULL, NULL);

  if (self->has_loaded)
    return;

  self->has_loaded = TRUE;

  if (!self->active_signature && self->composer_from) {
    StampAccount *account = stamp_composer_from_get_account (self->composer_from);
    g_autofree char *path = g_strdup_printf ("/org/tabos/stamp/mail/accounts/%s/", stamp_account_get_uid (account));
    g_autoptr (GSettings) account_settings = g_settings_new_with_path ("org.tabos.stamp.mail.accounts", path);
    g_autofree char *default_uid = g_settings_get_string (account_settings, STAMP_PREFS_MAIL_DEFAULT_SIGNATURE);

    if (default_uid && default_uid[0]) {
      GList *sigs = stamp_session_get_signatures (stamp_session_get_default ());

      for (GList *iter = sigs; iter && iter->data; iter = g_list_next (iter)) {
        StampSignature *sig = (StampSignature *)iter->data;

        if (g_strcmp0 (e_source_get_uid (stamp_signature_get_source (sig)), default_uid) == 0) {
          insert_signature (self, sig);
          g_simple_action_set_state (self->signature_action, g_variant_new_string (stamp_signature_get_name (sig)));
          break;
        }
      }
    }
  }
}

static void
undo_cb (GSimpleAction *action,
         GVariant      *parameter,
         gpointer       user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  webkit_web_view_evaluate_javascript (WEBKIT_WEB_VIEW (self->webview), "document.execCommand('undo');", -1, NULL, NULL, NULL, NULL, NULL);
}

static void
redo_cb (GSimpleAction *action,
         GVariant      *parameter,
         gpointer       user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  webkit_web_view_evaluate_javascript (WEBKIT_WEB_VIEW (self->webview), "document.execCommand('redo');", -1, NULL, NULL, NULL, NULL, NULL);
}

static gboolean
on_context_menu (WebKitWebView       *web_view,
                 WebKitContextMenu   *context_menu,
                 WebKitHitTestResult *hit_test_result,
                 gpointer             user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  if (webkit_hit_test_result_context_is_editable (hit_test_result)) {
    WebKitEditorState *state;
    WebKitContextMenuItem *item;
    GSimpleAction *action;
    gboolean can_undo, can_redo;

    state = webkit_web_view_get_editor_state (WEBKIT_WEB_VIEW (self->webview));
    can_undo = webkit_editor_state_is_undo_available (state);
    can_redo = webkit_editor_state_is_redo_available (state);

    webkit_context_menu_prepend (context_menu, webkit_context_menu_item_new_separator ());

    action = g_simple_action_new ("redo", NULL);
    g_signal_connect (action, "activate", G_CALLBACK (redo_cb), self);
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), can_redo);
    item = webkit_context_menu_item_new_from_gaction (G_ACTION (action), _("Redo"), NULL);
    webkit_context_menu_prepend (context_menu, item);

    action = g_simple_action_new ("undo", NULL);
    g_signal_connect (action, "activate", G_CALLBACK (undo_cb), self);
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), can_undo);
    item = webkit_context_menu_item_new_from_gaction (G_ACTION (action), _("Undo"), NULL);
    webkit_context_menu_prepend (context_menu, item);

    return FALSE;
  }

  return FALSE;
}

static void
on_insert_link_response (AdwAlertDialog *dialog,
                         const gchar    *response,
                         gpointer        user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  if (g_strcmp0 (response, "insert") == 0) {
    GtkWidget *extra_child = adw_alert_dialog_get_extra_child (dialog);
    GtkWidget *text_entry = g_object_get_data (G_OBJECT (extra_child), "text-entry");
    GtkWidget *url_entry = g_object_get_data (G_OBJECT (extra_child), "url-entry");
    const gchar *text = gtk_editable_get_text (GTK_EDITABLE (text_entry));
    const gchar *url = gtk_editable_get_text (GTK_EDITABLE (url_entry));

    if (url && *url) {
      if (text && *text) {
        g_autofree char *html = g_strdup_printf ("<a href=\"%s\">%s</a>", url, text);

        stamp_web_view_execute_editor_command (self->webview, "insertHTML", html);
      } else {
        stamp_web_view_execute_editor_command (self->webview, "createLink", url);
      }
    }
  }
}

static void
on_insert_link_activated (GSimpleAction *action,
                          GVariant      *parameter,
                          gpointer       user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  GtkWidget *dialog;
  GtkWidget *box;
  GtkWidget *text_entry;
  GtkWidget *url_entry;

  dialog = GTK_WIDGET (adw_alert_dialog_new (_("Insert Link"), NULL));

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

  text_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (text_entry), _("Text"));
  gtk_box_append (GTK_BOX (box), text_entry);

  url_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (url_entry), _("URL"));
  gtk_box_append (GTK_BOX (box), url_entry);

  g_object_set_data (G_OBJECT (box), "text-entry", text_entry);
  g_object_set_data (G_OBJECT (box), "url-entry", url_entry);

  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), box);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("_Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "insert", _("_Insert"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "insert", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "insert");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

  g_signal_connect (dialog, "response", G_CALLBACK (on_insert_link_response), self);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static const GActionEntry stamp_composer_action_entries[] = {
  { "bold", on_edit_activate, "s", "''", NULL},
  { "italic", on_edit_activate, "s", "''", NULL},
  { "underline", on_edit_activate, "s", "''", NULL},
  { "strikethrough", on_edit_activate, "s", "''", NULL},
  { "bullet-list", on_edit_activate, "s", "''", NULL},
  { "numbered-list", on_edit_activate, "s", "''", NULL},
  { "insert-link", on_insert_link_activated },
  { "send", on_send_activated },
  { "remove-format", on_remove_format_activated },
  { "add-attachment", on_add_attachment_activated },
};

static gboolean
on_accept (GtkDropTarget *self,
           GdkDrop       *drop,
           gpointer       user_data)
{
  return TRUE;
}

static gboolean
on_drop (GtkDropTarget *target,
         const GValue  *value,
         gdouble        x,
         gdouble        y,
         gpointer       user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  GdkFileList *list = g_value_get_boxed (value);
  GSList *files;
  gint total;
  gint count = 0;
  GFile **file_arr;
  AttachmentBatch *batch;

  if (!G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    return FALSE;

  files = gdk_file_list_get_files (list);
  total = g_slist_length (files);
  if (total == 0)
    return TRUE;

  file_arr = g_new0 (GFile *, total);

  for (GSList *iter = files; iter && iter->data; iter = g_slist_next (iter))
    file_arr[count++] = g_object_ref (G_FILE (iter->data));

  batch = g_new0 (AttachmentBatch, 1);
  batch->self = self;
  batch->total = count;
  batch->current = 0;
  batch->files = file_arr;

  process_attachment_file (batch);

  return TRUE;
}

static void
build_signature_menu (StampComposer *self)
{
  GMenu *menu = g_menu_new ();
  GMenu *section;
  GMenuItem *item;
  GList *sigs;

  section = g_menu_new ();
  item = g_menu_item_new (_("No Signature"), "composer.signature");
  g_menu_item_set_attribute_value (item, "target", g_variant_new_string (""));
  g_menu_append_item (section, item);
  g_menu_append_section (menu, NULL, G_MENU_MODEL (section));

  section = g_menu_new ();

  sigs = stamp_session_get_signatures (stamp_session_get_default ());
  for (GList *iter = sigs; iter && iter->data; iter = g_list_next (iter)) {
    StampSignature *sig = (StampSignature *)iter->data;

    item = g_menu_item_new (stamp_signature_get_name (sig), "composer.signature");
    g_menu_item_set_attribute_value (item, "target", g_variant_new_string (stamp_signature_get_name (sig)));
    g_menu_append_item (section, item);
  }
  g_menu_append_section (menu, NULL, G_MENU_MODEL (section));

  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (self->signature), G_MENU_MODEL (menu));

  self->signature_action = g_simple_action_new_stateful ("signature", G_VARIANT_TYPE_STRING, g_variant_new_string (""));
  g_signal_connect (self->signature_action, "activate", G_CALLBACK (on_signature_activated), self);
  g_action_map_add_action (G_ACTION_MAP (self->actions), G_ACTION (self->signature_action));
}

static void
on_security_toggle_toggled (GtkWidget *toggle,
                            gpointer   user_data)
{
  StampComposer *self = user_data;
  gboolean active = FALSE;

  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (self->pgp_sign)) ||
      gtk_check_button_get_active (GTK_CHECK_BUTTON (self->pgp_encrypt)) ||
      gtk_check_button_get_active (GTK_CHECK_BUTTON (self->smime_sign)) ||
      gtk_check_button_get_active (GTK_CHECK_BUTTON (self->smime_encrypt)))
    active = TRUE;

  gtk_widget_set_visible (self->security_is_active, active);
}

static void
stamp_composer_init (StampComposer *self)
{
  GBytes *template;
  GtkListItemFactory *factory;
  GtkListItemFactory *selected_factory;
  WebKitUserContentManager *manager;
  g_autofree char *tmp = NULL;
  GtkShortcut *shortcut;
  GtkEventController *controller;
  GtkDropTarget *drop = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  GAction *action;

  gtk_widget_init_template (GTK_WIDGET (self));

  self->type = STAMP_COMPOSER_NEW;
  self->attachments = NULL;
  self->cancellable = g_cancellable_new ();
  self->webview = stamp_webview_new ();

  g_signal_connect (self->webview, "context-menu", G_CALLBACK (on_context_menu), self);

  stamp_webview_set_editable (self->webview);
  gtk_widget_set_focusable (GTK_WIDGET (self->webview), TRUE);
  adw_bin_set_child (ADW_BIN (self->webview_bin), GTK_WIDGET (self->webview));

  template = g_resources_lookup_data ("/org/tabos/stamp/blank-message-template.html", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  webkit_web_view_load_html (WEBKIT_WEB_VIEW (self->webview), g_bytes_get_data (template, NULL), NULL);

  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (on_setup_selected), self);
  g_signal_connect (factory, "bind", G_CALLBACK (on_bind_selected), self);

  gtk_drop_down_set_factory (GTK_DROP_DOWN (self->from), factory);

  selected_factory = gtk_signal_list_item_factory_new ();
  g_signal_connect_object (selected_factory, "setup", G_CALLBACK (on_from_setup), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (selected_factory, "bind", G_CALLBACK (on_from_bind), self, G_CONNECT_DEFAULT);
  gtk_drop_down_set_list_factory (GTK_DROP_DOWN (self->from), selected_factory);

  g_signal_connect_object (self->from, "notify::selected-item", G_CALLBACK (on_from_selected_item), self, G_CONNECT_DEFAULT);
  load_from_combobox (self);

  gtk_widget_grab_focus (self->to);

  manager = webkit_web_view_get_user_content_manager (WEBKIT_WEB_VIEW (self->webview));
  webkit_user_content_manager_register_script_message_handler (manager, "dirty", NULL);
  g_signal_connect_object (manager, "script-message-received::dirty", G_CALLBACK (on_dirty_message), self, G_CONNECT_DEFAULT);

  g_signal_connect (self->webview, "load-changed", G_CALLBACK (on_load_changed), self);

  /* self->auto_save_draft_handler = g_timeout_add_seconds (3, auto_save_draft, self); */

  self->cancellable = g_cancellable_new ();
  self->active_signature = NULL;
  self->has_loaded = FALSE;

  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions),
                                   stamp_composer_action_entries,
                                   G_N_ELEMENTS (stamp_composer_action_entries),
                                   self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "composer", G_ACTION_GROUP (self->actions));

  action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "send");
  g_object_bind_property (self->to, "has-entries", action, "enabled", G_BINDING_SYNC_CREATE);
  build_signature_menu (self);

  controller = gtk_shortcut_controller_new ();
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  shortcut = gtk_shortcut_new (gtk_shortcut_trigger_parse_string ("<primary>Return"), gtk_named_action_new ("composer.send"));
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (controller), shortcut);

  shortcut = gtk_shortcut_new (gtk_shortcut_trigger_parse_string ("Escape"), gtk_named_action_new ("window.close"));
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (controller), shortcut);

  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drop), GTK_PHASE_CAPTURE);
  g_signal_connect (drop, "accept", G_CALLBACK (on_accept), self);
  g_signal_connect (drop, "drop", G_CALLBACK (on_drop), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drop));

  g_settings_bind (STAMP_SETTINGS, STAMP_PREFS_COMPOSER_WIDTH, self, "default-width", G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (STAMP_SETTINGS, STAMP_PREFS_COMPOSER_HEIGHT, self, "default-height", G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (STAMP_SETTINGS, STAMP_PREFS_COMPOSER_MAXIMIZED, self, "maximized", G_SETTINGS_BIND_DEFAULT);
}

static void
on_close_button_clicked (GtkWidget *button,
                         gpointer   user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  gtk_window_close (GTK_WINDOW (self));
}

static void
on_draft_get_body_html (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  StampComposer *self = user_data;
  StampWebView *web_view = STAMP_WEB_VIEW (source_object);
  g_autoptr (GError) error = NULL;
  g_autofree char *body_plain = NULL;
  g_autofree char *body = stamp_webview_get_body_html_finish (web_view, res, &body_plain, &error);
  CamelMimeMessage *mime_message;
  CamelInternetAddress *sender;
  CamelInternetAddress *recipient;
  const gchar *name;
  const gchar *mail;

  if (!body) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Failed to get HTML content for mail: %s", error->message);
    return;
  }

  mime_message = build_message (self, body, body_plain);

  name = stamp_composer_from_get_name (self->composer_from);
  mail = stamp_composer_from_get_mail (self->composer_from);

  sender = build_sender (mime_message, name, mail);
  recipient = build_recipients (self, mime_message);

  if (stamp_account_save_draft (stamp_composer_from_get_account (self->composer_from), self->draft_uid, mime_message, sender, recipient)) {
    gtk_window_destroy (GTK_WINDOW (self));
  }
}

static void
on_draft_response (AdwAlertDialog *dialog,
                   gchar          *response,
                   gpointer        user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);

  if (g_strcmp0 (response, "save-draft") == 0) {
    if (self->is_dirty)
      stamp_webview_get_body_html (self->webview, self->cancellable, on_draft_get_body_html, self);
    else
      gtk_window_destroy (GTK_WINDOW (self));
  } else if (g_strcmp0 (response, "close") == 0) {
    stamp_account_remove_draft (stamp_composer_from_get_account (self->composer_from), self->draft_uid);
    gtk_window_destroy (GTK_WINDOW (self));
  }
}

static gboolean
on_close_request (GtkWindow *source,
                  gpointer   user_data)
{
  StampComposer *self = STAMP_COMPOSER (user_data);
  AdwDialog *dialog;

  g_clear_handle_id (&self->autosave_source_id, g_source_remove);

  if (!self->is_dirty)
    return FALSE;

  dialog = adw_alert_dialog_new (_("Changes detected"), _("You have modified this mail and closing this window will lead to loss of those changes. How do you want to proceed?"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("Close"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "save-draft", _("Save Draft"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "continue", _("Continue Editing"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "continue", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "close", ADW_RESPONSE_DESTRUCTIVE);

  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "continue");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "save-draft");

  g_signal_connect (dialog, "response", G_CALLBACK (on_draft_response), self);
  adw_dialog_present (dialog, GTK_WIDGET (source));

  return TRUE;
}

static void
stamp_composer_get_property (GObject    *object,
                             guint       property_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  StampComposer *self = STAMP_COMPOSER (object);

  switch ((StampComposerProps)property_id) {
    case PROP_ACCOUNT:
      g_value_set_object (value, self->composer_from);
      break;
  }
}

static void
stamp_composer_set_property (GObject      *object,
                             guint         property_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  StampComposer *self = STAMP_COMPOSER (object);

  switch ((StampComposerProps)property_id) {
    case PROP_ACCOUNT:
      stamp_composer_set_composer_from_by_account (self, g_value_get_object (value));
      break;
  }
}

static void
stamp_composer_dispose (GObject *object)
{
  StampComposer *self = STAMP_COMPOSER (object);

  g_clear_handle_id (&self->autosave_source_id, g_source_remove);
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_object (&self->orig_message);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (stamp_composer_parent_class)->dispose (object);
}

static void
stamp_composer_class_init (StampComposerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/composer/stamp-composer.ui");

  object_class->get_property = stamp_composer_get_property;
  object_class->set_property = stamp_composer_set_property;
  object_class->dispose = stamp_composer_dispose;

  gtk_widget_class_bind_template_child (widget_class, StampComposer, from);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, to);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, cc);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, bcc);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, webview_bin);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, send_button);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, subject);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, window_title);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, attachment_revealer);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, attachment_box);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, request_disposition);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, pgp_sign);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, pgp_encrypt);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, smime_sign);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, smime_encrypt);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, security_menu);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, security_is_active);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, security_popover);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, insert_link_button);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, toggle);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, toast_overlay);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, signature);
  gtk_widget_class_bind_template_child (widget_class, StampComposer, scrolled_window);

  gtk_widget_class_bind_template_callback (widget_class, on_close_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_close_request);
  gtk_widget_class_bind_template_callback (widget_class, on_subject_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_security_toggle_toggled);

  props[PROP_ACCOUNT] = g_param_spec_object ("account",
                                             NULL,
                                             NULL,
                                             STAMP_TYPE_ACCOUNT,
                                             G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (props), props);
}

GtkWidget *
stamp_composer_new (StampAccount *account)
{
  StampComposer *self = g_object_new (STAMP_TYPE_COMPOSER,
                                      "account", account,
                                      "application", GTK_APPLICATION (g_application_get_default ()),
                                      NULL);

  return GTK_WIDGET (self);
}

static gboolean
stamp_add_addresses_to_completion (StampComposer        *self,
                                   GtkWidget            *wrap_box,
                                   CamelInternetAddress *address)
{
  gint len;

  len = camel_address_length (CAMEL_ADDRESS (address));

  for (gint idx = 0; idx < len; idx++) {
    GtkWidget *tag = stamp_tag_new (stamp_composer_from_get_account (self->composer_from));
    const gchar *ia_name;
    const gchar *ia_address;
    const gchar *to = NULL;

    camel_internet_address_get (address, idx, &ia_name, &ia_address);
    if (g_strcmp0 (ia_name, "") != 0) {
      to = ia_name;
    } else {
      to = ia_address;
    }

    if (!stamp_contact_completion_contains_address (STAMP_CONTACT_COMPLETION (wrap_box), ia_address)) {
      stamp_tag_set_label (STAMP_TAG (tag), to);
      stamp_tag_set_mail (STAMP_TAG (tag), ia_address);
      stamp_tag_set_show_button (STAMP_TAG (tag), TRUE);

      stamp_contact_completion_add_tag (STAMP_CONTACT_COMPLETION (wrap_box), STAMP_TAG (tag));
    }
  }

  return len != 0;
}

static void
remove_own_address (StampComposer        *self,
                    CamelInternetAddress *address)
{
  const gchar *own_mail;
  gint len;
  gint idx;

  if (!self->composer_from)
    return;

  own_mail = stamp_composer_from_get_mail (self->composer_from);

  len = camel_address_length (CAMEL_ADDRESS (address));

  for (idx = 0; idx < len; idx++) {
    const gchar *ia_name;
    const gchar *ia_address;

    camel_internet_address_get (address, idx, &ia_name, &ia_address);
    if (g_strcmp0 (ia_address, own_mail) == 0) {
      break;
    }
  }

  if (idx != len) {
    camel_address_remove (CAMEL_ADDRESS (address), idx);
  }
}

static void
stamp_composer_set_quote_content (StampComposer          *self,
                                  StampComposerType       type,
                                  const gchar            *uid,
                                  StampWebView           *webview,
                                  const CamelMessageInfo *info,
                                  CamelMimeMessage       *message,
                                  gchar                  *content_to_quote)
{
  const gchar *subject = camel_message_info_get_subject (info);

  self->type = type;
  self->orig_message = g_object_ref (message);

  if (subject) {
    g_autofree char *new_subject = NULL;

    if (type == STAMP_COMPOSER_REPLY || type == STAMP_COMPOSER_REPLY_ALL) {
      g_autofree char *upper_subject = g_ascii_strup (subject, -1);

      if (g_str_has_prefix (upper_subject, "RE: ")) {
        new_subject = g_strdup (subject);
      } else {
        new_subject = g_strdup_printf ("Re: %s", subject);
      }
    } else {
      new_subject = g_strdup (subject);
    }

    gtk_editable_set_text (GTK_EDITABLE (self->subject), new_subject);
  }

  if (content_to_quote) {
    g_autoptr (GString) message_content = g_string_new ("");

    if (type == STAMP_COMPOSER_DRAFT) {
      CamelInternetAddress *to = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO);
      CamelInternetAddress *cc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC);
      CamelInternetAddress *bcc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_BCC);

      self->draft_uid = g_strdup (uid);

      if (to)
        stamp_add_addresses_to_completion (self, self->to, to);

      if (cc)
        if (stamp_add_addresses_to_completion (self, self->cc, cc))
          gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->toggle), TRUE);

      if (bcc)
        if (stamp_add_addresses_to_completion (self, self->bcc, bcc))
          gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->toggle), TRUE);

      g_string_append (message_content, content_to_quote);
    } else {
      CamelInternetAddress *from = camel_mime_message_get_from (message);
      CamelInternetAddress *to = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO);
      CamelInternetAddress *cc = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC);
      g_autofree char *formatted_from = camel_address_format (CAMEL_ADDRESS (from));
      g_autoptr (GDateTime) when = NULL;
      g_autofree char *date_received = NULL;
      gchar *date_format = _("%a, %b %-e, %Y at %-l:%M %p");

      message_content = g_string_append (message_content, "<br/><br/>");

      when = g_date_time_new_from_unix_utc (camel_message_info_get_date_received (info));
      date_received = g_date_time_format (when, date_format);

      if (type == STAMP_COMPOSER_REPLY || type == STAMP_COMPOSER_REPLY_ALL) {
        CamelInternetAddress *reply_to = camel_mime_message_get_reply_to (message);
        gchar *who;

        if (reply_to) {
          remove_own_address (self, reply_to);
          stamp_add_addresses_to_completion (self, self->to, reply_to);
        } else {
          remove_own_address (self, from);
          stamp_add_addresses_to_completion (self, self->to, from);
        }

        if (type == STAMP_COMPOSER_REPLY_ALL) {
          remove_own_address (self, to);
          stamp_add_addresses_to_completion (self, self->to, to);
        }

        if (cc && type == STAMP_COMPOSER_REPLY_ALL) {
          if (stamp_add_addresses_to_completion (self, self->cc, cc))
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->toggle), TRUE);
        }

        who = formatted_from;

        g_string_append_printf (message_content, _("On %s, %s wrote:"), date_received, who);
        message_content = g_string_append (message_content, "<br/>");
        g_string_append_printf (message_content, "<blockquote type=\"cite\">%s</blockquote>", content_to_quote);
      } else if (type == STAMP_COMPOSER_FORWARD) {
        g_string_append_printf (message_content, _("---------- Forwarded message ----------"));
        g_string_append_printf (message_content, "<br/><br/>");
        g_string_append_printf (message_content, _("From: %s<br/>"), formatted_from);
        g_string_append_printf (message_content, _("Subject: %s<br/>"), camel_message_info_get_subject (info));
        g_string_append_printf (message_content, _("Date: %s<br/>"), date_received);

        g_string_append_printf (message_content, "<br/><br/>");
        g_string_append_printf (message_content, "%s", content_to_quote);
      }
    }

    stamp_webview_copy_resources (webview, self->webview);
    stamp_web_view_set_body_content (self->webview, message_content->str);
  }

  gtk_widget_grab_focus (GTK_WIDGET (self->webview));
}

GtkWidget *
stamp_composer_new_with_quote (StampComposerType       type,
                               const gchar            *uid,
                               StampAccount           *account,
                               StampWebView           *webview,
                               const CamelMessageInfo *info,
                               CamelMimeMessage       *mime_message,
                               gchar                  *content_to_quote)
{
  GtkWidget *composer = g_object_new (STAMP_TYPE_COMPOSER, "account", account, "application", GTK_APPLICATION (g_application_get_default ()), NULL);

  stamp_composer_set_quote_content (STAMP_COMPOSER (composer),
                                    type,
                                    uid,
                                    webview,
                                    info,
                                    mime_message,
                                    content_to_quote);

  return composer;
}

void
stamp_composer_set_to (StampComposer *self,
                       gchar         *to)
{
  g_autoptr (CamelInternetAddress) to_address = NULL;

  if (!to || strlen (to) == 0)
    return;

  to_address = camel_internet_address_new ();
  camel_internet_address_add (to_address, "", to);

  if (to_address)
    stamp_add_addresses_to_completion (self, self->to, to_address);
}

void
stamp_composer_set_subject (StampComposer *self,
                            gchar         *subject)
{
  gtk_editable_set_text (GTK_EDITABLE (self->subject), subject);
}

void
stamp_composer_set_body (StampComposer *self,
                         gchar         *body)
{
  stamp_web_view_set_body_content (self->webview, body);

  gtk_widget_grab_focus (GTK_WIDGET (self->webview));
}

void
stamp_composer_add_attachment (StampComposer *self,
                               gchar         *path)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  GtkWidget *button;

  if (!file) {
    g_warning ("%s: Could not get file for path %s, abort", G_STRFUNC, path);
    return;
  }

  button = stamp_attachment_button_new_from_file (file);

  self->attachments = g_list_append (self->attachments, button);
  adw_wrap_box_append (ADW_WRAP_BOX (self->attachment_box), button);
  attach_box_update_max_height (GTK_SCROLLED_WINDOW (self->scrolled_window), ADW_WRAP_BOX (self->attachment_box));
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->attachment_revealer), TRUE);
}
