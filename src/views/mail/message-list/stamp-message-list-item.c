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

#include "stamp-message-list-item.h"

#include <adwaita.h>
#include <camel/camel.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <libecal/libecal.h>
#include <libical-glib/libical-glib.h>
#include <nss.h>
#include <webkit/webkit.h>

#include "stamp-attachment-button.h"
#include "stamp-message-header.h"
#include "stamp-message-list.h"
#include "stamp-mime-parser.h"
#include "stamp-session.h"
#include "stamp-settings.h"
#include "stamp-webview.h"

#define USER_AGENT ("Stamp " PACKAGE_VERSION)

#define LIBICAL_GLIB_UNSTABLE_API 1
struct _StampMessageListItem {
  GtkListBoxRow parent_instance;

  GtkWidget *header;
  GtkWidget *secondary_revealer;
  GtkWidget *vcard_banner;
  GtkWidget *blocked_images_banner;
  GtkWidget *signature_banner;
  GtkWidget *encryption_banner;
  GtkWidget *attachment_flow_box;
  GtkWidget *attachment_info_label;
  GtkWidget *save_all_button;
  GtkWidget *disposition_banner;

  GtkBox *web_view_box;
  StampWebView *web_view;
  StampAccount *account;

  StampMimeParser *parser;

  const CamelMessageInfo *message_info;
  gchar *message_content;
  gchar *signature_details;
  CamelCipherValiditySign signature_status;
  gchar *disposition_notification_to;
  gboolean message_is_html;
  gboolean expanded;
  gboolean message_loaded;
  gboolean web_view_loaded;
  gboolean find_connected;
  gchar *pending_search;
  StampMessageListItemSearchDone search_done;
  gpointer search_done_data;
  GCancellable *cancellable;
  CamelMimeMessage *message;
  ICalComponent *calendar;
  StampMimeListUnsubscribe *unsubscribe;

  GSimpleActionGroup *actions;
  StampComposerType type;
  gchar *save_img;
};

G_DEFINE_FINAL_TYPE (StampMessageListItem, stamp_message_list_item, GTK_TYPE_LIST_BOX_ROW);

typedef enum {
  PROP_ACCOUNT = 1,
  PROP_MESSAGE_INFO,
  PROP_EXPANDED,
} StampMessageListItemProps;

static GParamSpec *properties[PROP_EXPANDED + 1];

static void
update_actions (StampMessageListItem *self)
{
  GAction *action;
  guint32 flags = camel_message_info_get_flags (self->message_info);
  gboolean flagged = (flags & CAMEL_MESSAGE_FLAGGED) != 0;
  gboolean read = (flags & CAMEL_MESSAGE_SEEN) != 0;

  action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "mark-unflag");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), flagged);
  action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "mark-flag");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), !flagged);

  action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "mark-unread");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), read);
  action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "mark-read");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), !read);

  action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "unsubscribe");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), self->unsubscribe != NULL);
}

static void
ensure_web_view (StampMessageListItem *self);

static void
update_encryption_banner (StampMessageListItem *self,
                          const GList          *encryptions)
{
  AdwBanner *banner = ADW_BANNER (self->encryption_banner);

  adw_banner_set_revealed (banner, FALSE);
  gtk_widget_set_tooltip_text (self->encryption_banner, NULL);
  if (!encryptions)
    return;

  for (const GList *iter = encryptions; iter; iter = iter->next) {
    const StampMimeEncryption *encryption = iter->data;

    if (!encryption->success || encryption->status != STAMP_MIME_ENCRYPTION_VALID) {
      adw_banner_set_title (banner, _("Message could not be decrypted"));
      gtk_widget_set_tooltip_text (self->encryption_banner, encryption->error_message);
      adw_banner_set_revealed (banner, TRUE);
      return;
    }
  }

  adw_banner_set_title (banner, _("Message decrypted"));
  adw_banner_set_revealed (banner, TRUE);
}

static void
open_message (StampMessageListItem *self)
{
  StampMimeBody *body;
  const GList *signatures;
  const GList *encryptions;
  StampMimeCalendar *calendar;
  GList *list_unsubscribe;
  const gchar *address = camel_medium_get_header (CAMEL_MEDIUM (self->message), "Disposition-Notification-To");
  const gchar *auth_as = camel_medium_get_header (CAMEL_MEDIUM (self->message), "X-MS-Exchange-Organization-AuthAs");
  const gchar *sender = camel_medium_get_header (CAMEL_MEDIUM (self->message), "Sender");
  GList *attachments;
  StampMessageList *message_list = STAMP_MESSAGE_LIST (gtk_widget_get_ancestor (GTK_WIDGET (self), STAMP_TYPE_MESSAGE_LIST));

  if (!message_list)
    return;

  ensure_web_view (self);
  self->web_view_loaded = FALSE;
  g_clear_object (&self->parser);

  if (!address)
    address = camel_medium_get_header (CAMEL_MEDIUM (self->message), "Return-Receipt-To");

  if (address) {
    while (camel_mime_is_lwsp (*address))
      address++;
  }

  if (address && *address) {
    g_autofree char *tmp = NULL;

    self->disposition_notification_to = g_strdup (address);
    tmp = g_strdup_printf (_("Sender %s wants a read receipt"), self->disposition_notification_to);
    adw_banner_set_title (ADW_BANNER (self->disposition_banner), tmp);

    adw_banner_set_revealed (ADW_BANNER (self->disposition_banner), TRUE);
  }

  if (auth_as) {
    if (g_strcmp0 (auth_as, "Internal") == 0) {
      stamp_web_view_load_images (self->web_view);
      stamp_message_header_set_internal (STAMP_MESSAGE_HEADER (self->header), TRUE);
      stamp_message_list_set_external (message_list, FALSE);
    } else {
      stamp_message_list_set_external (message_list, TRUE);
    }
  } else {
    stamp_message_list_set_external (message_list, FALSE);
  }

  stamp_message_header_set_sender (STAMP_MESSAGE_HEADER (self->header), sender);

  self->parser = stamp_mime_parser_new (CAMEL_SESSION (stamp_session_get_default ()));
  stamp_mime_parser_parse (self->parser, self->message, self->cancellable, NULL);

  signatures = stamp_mime_parser_get_signatures (self->parser);
  encryptions = stamp_mime_parser_get_encryptions (self->parser);
  if (signatures) {
    StampMimeSignature *signature = signatures->data;

    if (signature->status != STAMP_MIME_SIGNATURE_NONE) {
      g_autofree char *tmp = NULL;

      if (signature->description) {
        g_autoptr (GString) str = g_string_new ("");

        tmp = g_strdup (signature->description);
        g_strdelimit (tmp, "\r\n", ' ');
        adw_banner_set_title (ADW_BANNER (self->signature_banner), g_strstrip (tmp));
        adw_banner_set_revealed (ADW_BANNER (self->signature_banner), TRUE);

        for (GList *iter = signature->signers; iter && iter->data; iter = g_list_next (iter)) {
          StampMimeSignerInfo *info = iter->data;

          g_string_append_printf (str, "%s <%s>\n", info->name, info->email);
        }

        self->signature_details = g_strdup (str->str);
      }
    }
  }

  update_encryption_banner (self, encryptions);

  calendar = stamp_mime_parser_get_invitations (self->parser);
  if (calendar) {
    ICalTime *time;

    self->calendar = g_object_ref (calendar->ical);
    time = i_cal_component_get_dtstart (self->calendar);
    if (time) {
      g_autofree char *tmp = g_strdup_printf ("%s: %d.%d.%d %.2d:%.2d",
                                              i_cal_component_get_summary (self->calendar),
                                              i_cal_time_get_day (time),
                                              i_cal_time_get_month (time),
                                              i_cal_time_get_year (time),
                                              i_cal_time_get_hour (time),
                                              i_cal_time_get_minute (time));
      adw_banner_set_title (ADW_BANNER (self->vcard_banner), tmp);
      adw_banner_set_revealed (ADW_BANNER (self->vcard_banner), TRUE);

      if (g_strcmp0 (calendar->method, "REPLY") == 0)
        adw_banner_set_button_label (ADW_BANNER (self->vcard_banner), NULL);
    }
  }

  list_unsubscribe = stamp_mime_parser_get_list_unsubscribe (self->parser);
  if (list_unsubscribe) {
    self->unsubscribe = list_unsubscribe->data;
  }

  body = stamp_mime_parser_get_body (self->parser);
  if (body && body->text) {
    self->message_content = g_strdup (body->text);
    self->message_is_html = body->is_html;

    if (self->message_is_html && body->is_html) {
      /*char *html_with_images = stamp_mime_parser_embed_inline_images (parser, self->message_content);
      if (html_with_images) {
        g_clear_pointer (&self->message_content, g_free);
        self->message_content = html_with_images;
      }*/
    }
  }

  attachments = stamp_mime_parser_get_attachments (self->parser);
  if (attachments) {
    guint count = 0;
    gsize total_size = 0;
    g_autofree char *label = NULL;

    for (GList *iter = attachments; iter && iter->data; iter = g_list_next (iter)) {
      StampMimeAttachment *att = iter->data;
      GtkWidget *button = stamp_attachment_button_new_from_data (att->filename, att->mime_type, att->size, att->data, FALSE);

      adw_wrap_box_append (ADW_WRAP_BOX (self->attachment_flow_box), button);

      count++;
      total_size += att->size;
    }

    label = g_strdup_printf (ngettext ("%d Attachment (%s)", "%d Attachments (%s)", count), count, g_format_size (total_size));
    gtk_label_set_text (GTK_LABEL (self->attachment_info_label), label);
  }

  gtk_widget_set_visible (self->attachment_flow_box, attachments != NULL);
  gtk_widget_set_visible (self->attachment_info_label, attachments && g_list_length (attachments) > 1);
  gtk_widget_set_visible (self->save_all_button, attachments && g_list_length (attachments) > 1);
  g_list_free (attachments);

  if (self->message_content) {
    if (self->message_is_html) {
      stamp_webview_load_html (self->web_view, self->message_content);
    } else {
      stamp_webview_load_plain_text (self->web_view, self->message_content);
    }
  }

  update_actions (self);
}

static void
on_get_message (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  StampMessageListItem *self;
  g_autoptr (GError) error = NULL;
  CamelFolder *folder;
  g_autoptr (CamelMimeMessage) message = NULL;

  folder = CAMEL_FOLDER (source);
  message = camel_folder_get_message_finish (folder, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("%s: Could not get message: %s", G_STRFUNC, error->message);
    return;
  }

  self = STAMP_MESSAGE_LIST_ITEM (user_data);
  if (!self->message_loaded)
    return;

  ensure_web_view (self);

  if ((camel_folder_get_flags (folder) & CAMEL_FOLDER_IS_JUNK) == 0 &&
      (camel_folder_get_flags (folder) & CAMEL_FOLDER_IS_TRASH) == 0 &&
      g_settings_get_boolean (STAMP_SETTINGS_MAIL, STAMP_PREFS_MAIL_ALWAYS_SHOW_IMAGES))
    stamp_web_view_load_images (self->web_view);

  g_set_object (&self->message, message);

  if (self->message)
    open_message (self);
}

static void
start_get_message (StampMessageListItem *self,
                   GAsyncReadyCallback   callback,
                   gpointer              user_data)
{
  CamelFolderSummary *summary;
  CamelFolder *folder;

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  self->cancellable = g_cancellable_new ();

  g_object_get (G_OBJECT (self->message_info), "summary", &summary, NULL);

  folder = camel_folder_summary_get_folder (summary);

  camel_folder_get_message (folder, camel_message_info_get_uid (self->message_info), G_PRIORITY_DEFAULT, self->cancellable, on_get_message, self);
}

void
stamp_message_list_item_set_expanded (StampMessageListItem *self,
                                      gboolean              expanded)
{
  gtk_widget_set_visible (self->secondary_revealer, TRUE);
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->secondary_revealer), expanded);

  self->expanded = expanded;
  stamp_message_header_set_collapsed (STAMP_MESSAGE_HEADER (self->header), expanded);

  if (expanded) {
    if (!self->message_loaded) {
      start_get_message (self, NULL, NULL);
      self->message_loaded = TRUE;
    }

    gtk_widget_add_css_class (GTK_WIDGET (self), "expanded");
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self), "expanded");
  }
}

static void
stamp_message_list_item_get_property (GObject    *object,
                                      guint       property_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (object);

  switch ((StampMessageListItemProps)property_id) {
    case PROP_MESSAGE_INFO:
      g_value_set_object (value, (GObject *)self->message_info);
      break;
    case PROP_ACCOUNT:
      g_value_set_object (value, self->account);
      break;
    case PROP_EXPANDED:
      g_value_set_boolean (value, gtk_revealer_get_reveal_child (GTK_REVEALER (self->secondary_revealer)));
      break;
  }
}

static void
stamp_message_list_item_set_property (GObject      *object,
                                      guint         property_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (object);

  switch ((StampMessageListItemProps)property_id) {
    case PROP_ACCOUNT:
      self->account = g_value_get_object (value);
      stamp_message_header_set_account (STAMP_MESSAGE_HEADER (self->header), self->account);
      break;
    case PROP_MESSAGE_INFO:
      self->message_info = g_value_get_object (value);
      break;
    case PROP_EXPANDED:
      stamp_message_list_item_set_expanded (self, g_value_get_boolean (value));
      break;
  }
}

static void
on_open_save_all_folder (AdwToast *toast,
                         gpointer  user_data)
{
  GFile *file = G_FILE (user_data);
  GtkWindow *window = g_object_get_data (G_OBJECT (toast), "open-folder-window");
  g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (file);

  gtk_file_launcher_open_containing_folder (launcher, window, NULL, NULL, NULL);
}

static void
on_save_all_folder_selected (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  g_autoptr (GtkFileDialog) dialog = GTK_FILE_DIALOG (source);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) folder = NULL;
  GList *attachments;
  GFile *first_file = NULL;

  folder = gtk_file_dialog_select_folder_finish (dialog, res, &error);
  if (error) {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;

    g_warning ("%s: %s", G_STRFUNC, error->message);
    return;
  }

  attachments = stamp_mime_parser_get_attachments (self->parser);
  for (GList *iter = attachments; iter; iter = g_list_next (iter)) {
    StampMimeAttachment *att = iter->data;
    g_autoptr (GFile) file = g_file_get_child (folder, att->filename);
    g_autoptr (GError) save_error = NULL;

    if (!first_file)
      first_file = g_object_ref (file);

    if (!g_file_replace_contents (file,
                                  g_bytes_get_data (att->data, NULL),
                                  g_bytes_get_size (att->data),
                                  NULL, FALSE, G_FILE_CREATE_NONE,
                                  NULL, NULL, &save_error))
      g_warning ("%s: Could not save '%s': %s", G_STRFUNC, att->filename, save_error->message);
  }

  g_list_free (attachments);

  if (first_file) {
    AdwToastOverlay *overlay = ADW_TOAST_OVERLAY (gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TOAST_OVERLAY));

    if (overlay) {
      AdwToast *toast = adw_toast_new (_("Attachments Saved"));

      adw_toast_set_button_label (toast, _("Open Folder"));
      g_object_set_data (G_OBJECT (toast), "open-folder-window", GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))));
      g_signal_connect_data (toast, "button-clicked", G_CALLBACK (on_open_save_all_folder), first_file, (GClosureNotify)g_object_unref, G_CONNECT_DEFAULT);
      adw_toast_overlay_add_toast (overlay, toast);
    } else {
      g_object_unref (first_file);
    }
  }
}

static void
on_save_all (GtkButton *button,
             gpointer   user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_set_title (dialog, _("Select destination folder"));
  gtk_file_dialog_select_folder (dialog, GTK_WINDOW (root), NULL, on_save_all_folder_selected, self);
}

static void
on_row_clicked (GObject  *object,
                gint      n_press,
                gdouble   x,
                gdouble   y,
                gpointer  user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  if (self->expanded && self->web_view) {
    GtkGestureClick *gesture = GTK_GESTURE_CLICK (object);
    GtkWidget *gesture_widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    graphene_rect_t web_view_bounds;

    if (gtk_widget_compute_bounds (GTK_WIDGET (self->web_view), gesture_widget, &web_view_bounds) &&
        x >= web_view_bounds.origin.x && x <= web_view_bounds.origin.x + web_view_bounds.size.width &&
        y >= web_view_bounds.origin.y && y <= web_view_bounds.origin.y + web_view_bounds.size.height)
      return;
  }

  stamp_message_list_item_set_expanded (self, !self->expanded);
}

static void
on_mouse_target_changed (WebKitWebView       *web_view,
                         WebKitHitTestResult *hit_test_result,
                         guint                modifiers,
                         gpointer             user_data)
{
  StampMessageList *message_list = STAMP_MESSAGE_LIST (gtk_widget_get_ancestor (GTK_WIDGET (user_data), STAMP_TYPE_MESSAGE_LIST));

  if (webkit_hit_test_result_context_is_link (hit_test_result)) {
    stamp_message_list_hovering_over_link (message_list, webkit_hit_test_result_get_link_title (hit_test_result), webkit_hit_test_result_get_link_uri (hit_test_result));
  } else {
    stamp_message_list_hovering_over_link (message_list, NULL, NULL);
  }
}

static void
on_image_load_blocked (StampWebView *web_view,
                       gpointer      user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  adw_banner_set_revealed (ADW_BANNER (self->blocked_images_banner), TRUE);
}

static void
on_show_images (AdwBanner *banner,
                gpointer   user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  if (self->web_view)
    stamp_web_view_load_images (self->web_view);
  adw_banner_set_revealed (banner, FALSE);
}

static void
on_show_signatures (AdwBanner *banner,
                    gpointer   user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  AdwDialog *dialog = adw_alert_dialog_new (_("Signature Details"), self->signature_details);
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (g_application_get_default ()));

  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("Close"));
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "close");

  adw_dialog_present (dialog, GTK_WIDGET (window));
}

static void
stamp_message_list_item_send_rsvp (StampMessageListItem  *self,
                                   ICalParameterPartstat  stat)
{
  ICalProperty *prop;
  ICalComponent *event;
  GCancellable *cancellable = g_cancellable_new ();
  g_autoptr (EClient) client = NULL;
  g_autoptr (GError) local_error = NULL;
  ESourceRegistry *registry = e_source_registry_new_sync (cancellable, &local_error);
  StampMailService *service = stamp_account_get_mail_service (self->account);
  const gchar *collection_uid = e_source_get_parent (stamp_mail_service_get_source (service));
  GList *sources = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_CALENDAR);
  ESource *source = NULL;
  CamelInternetAddress *address = stamp_account_get_address (self->account);
  const gchar *name;
  const gchar *email;
  gboolean stat_set = FALSE;

  camel_internet_address_get (address, 0, &name, &email);

  for (GList *l = sources; l; l = l->next) {
    ESource *src = l->data;

    if (g_strcmp0 (e_source_get_parent (src), collection_uid) == 0 /* && g_strcmp0 (e_source_get_display_name (src), "Kalender") == 0*/) {
      source = g_object_ref (src);
      break;
    }
  }

  if (!source) {
    g_warning ("%s: Could not find source, abort", G_STRFUNC);
    return;
  }

  event = i_cal_component_get_first_component (self->calendar, I_CAL_VEVENT_COMPONENT);

  for (prop = i_cal_component_get_first_property (event, I_CAL_ATTENDEE_PROPERTY); prop; prop = i_cal_component_get_next_property (event, I_CAL_ATTENDEE_PROPERTY)) {
    const gchar *attendee = i_cal_property_get_attendee (prop);
    const gchar *attendee_email;

    if (g_str_has_prefix (attendee, "mailto:"))
      attendee_email = attendee + 7;
    else
      attendee_email = attendee;

    if (g_strcmp0 (attendee_email, email) == 0) {
      ICalParameter *p = i_cal_parameter_new_partstat (stat);

      i_cal_property_set_parameter (prop, p);
      stat_set = TRUE;
      break;
    }
  }

  if (!stat_set) {
    g_autofree char *mailto = g_strdup_printf ("mailto:%s", email);
    ICalProperty *attendee = i_cal_property_new_attendee (mailto);

    i_cal_property_add_parameter (attendee, i_cal_parameter_new_partstat (stat));
    i_cal_component_add_property (event, attendee);
  }

  client = e_cal_client_connect_sync (source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 30, cancellable, &local_error);
  if (!client) {
    g_warning ("%s: Could not create ECalClient: %s", G_STRFUNC, local_error->message);
    return;
  }

  i_cal_component_set_method (self->calendar, I_CAL_METHOD_REPLY);
  e_cal_client_receive_objects_sync (E_CAL_CLIENT (client), self->calendar, E_CAL_OPERATION_FLAG_NONE, cancellable, &local_error);
  if (local_error) {
    g_warning ("%s: Could not receive event: %s", G_STRFUNC, local_error->message);
    return;
  }
}

static void
on_rsvp_response (GtkWidget *dialog,
                  gchar     *response,
                  gpointer   user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  ICalParameterPartstat stat = I_CAL_PARTSTAT_NONE;

  if (g_strcmp0 (response, "accept") == 0) {
    stat = I_CAL_PARTSTAT_ACCEPTED;
  } else if (g_strcmp0 (response, "tentative") == 0) {
    stat = I_CAL_PARTSTAT_TENTATIVE;
  } else if (g_strcmp0 (response, "decline") == 0) {
    stat = I_CAL_PARTSTAT_DECLINED;
  }

  if (stat != I_CAL_PARTSTAT_NONE)
    stamp_message_list_item_send_rsvp (self, stat);
}

static void
on_rsvp (AdwBanner *banner,
         gpointer   user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  AdwDialog *dialog;
  const gchar *sender = camel_message_info_get_from (self->message_info);
  g_autofree char *body = g_strdup_printf (_("%s wants to know whether you can join this meeting"), sender);

  dialog = adw_alert_dialog_new (_("RVSP"), body);

  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "decline", _("Decline"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "decline", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "tentative", _("Tentative"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "accept", _("Accept"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "accept", ADW_RESPONSE_SUGGESTED);

  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_rsvp_response), self);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
stamp_message_list_item_dispose (GObject *object)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (object);

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  g_clear_pointer (&self->pending_search, g_free);
  g_clear_pointer (&self->message_content, g_free);
  g_clear_pointer (&self->signature_details, g_free);
  g_clear_pointer (&self->disposition_notification_to, g_free);
  g_clear_pointer (&self->save_img, g_free);

  g_clear_object (&self->message);
  g_clear_object (&self->calendar);
  g_clear_object (&self->parser);

  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_MESSAGE_LIST_ITEM);

  G_OBJECT_CLASS (stamp_message_list_item_parent_class)->dispose (object);
}

static void
on_send_disposition (AdwBanner *banner,
                     gpointer   user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  g_autoptr (CamelMimeMessage) message = NULL;
  g_autoptr (CamelDataWrapper) body = NULL;
  g_autoptr (CamelInternetAddress) recipient = NULL;
  g_autoptr (GDateTime) received_dt = NULL;
  g_autoptr (GDateTime) now_dt = NULL;
  g_autofree char *date_str = NULL;
  g_autofree char *time_str = NULL;
  g_autofree char *body_text = NULL;
  g_autofree char *read_subject = NULL;
  g_autoptr (CamelStream) stream = NULL;
  const gchar *subject;
  gint64 date_received;

  if (!self->disposition_notification_to || !*self->disposition_notification_to)
    return;

  subject = camel_message_info_get_subject (self->message_info);
  date_received = camel_message_info_get_date_received (self->message_info);

  received_dt = g_date_time_new_from_unix_utc (date_received);
  now_dt = g_date_time_new_now_local ();

  date_str = g_date_time_format (received_dt, "%d.%m.%Y");
  time_str = g_date_time_format (now_dt, "%H:%M");

  read_subject = g_strdup_printf (_("Read: %s"), subject ? subject : "");
  body_text = g_strdup_printf (_("Your message from %s has been opened on %s."), date_str, time_str);

  message = camel_mime_message_new ();
  camel_mime_message_set_subject (message, read_subject);
  camel_mime_message_set_date (message, CAMEL_MESSAGE_DATE_CURRENT, 0);
  camel_medium_set_header (CAMEL_MEDIUM (message), "User-Agent", USER_AGENT);

  body = camel_data_wrapper_new ();
  camel_data_wrapper_set_mime_type (body, "text/plain; charset=utf-8");

  stream = camel_stream_mem_new_with_buffer (body_text, strlen (body_text));
  camel_data_wrapper_construct_from_stream_sync (body, stream, self->cancellable, NULL);

  camel_medium_set_content (CAMEL_MEDIUM (message), CAMEL_DATA_WRAPPER (body));

  camel_mime_message_set_from (message, stamp_account_get_address (self->account));

  recipient = camel_internet_address_new ();
  camel_internet_address_add (recipient, NULL, self->disposition_notification_to);
  camel_mime_message_set_recipients (message, CAMEL_RECIPIENT_TYPE_TO, recipient);

  adw_banner_set_revealed (ADW_BANNER (self->disposition_banner), FALSE);
  g_clear_pointer (&self->disposition_notification_to, g_free);

  stamp_account_send_mail (self->account, message, stamp_account_get_address (self->account), recipient, FALSE, FALSE, NULL, NULL, NULL);
}

void
stamp_message_list_item_class_init (StampMessageListItemClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  g_type_ensure (STAMP_TYPE_WEBVIEW);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/message-list/stamp-message-list-item.ui");

  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, header);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, blocked_images_banner);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, disposition_banner);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, signature_banner);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, encryption_banner);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, vcard_banner);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, web_view_box);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, secondary_revealer);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, attachment_flow_box);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, attachment_info_label);
  gtk_widget_class_bind_template_child (widget_class, StampMessageListItem, save_all_button);

  gtk_widget_class_bind_template_callback (widget_class, on_row_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_show_images);
  gtk_widget_class_bind_template_callback (widget_class, on_show_signatures);
  gtk_widget_class_bind_template_callback (widget_class, on_rsvp);
  gtk_widget_class_bind_template_callback (widget_class, on_image_load_blocked);
  gtk_widget_class_bind_template_callback (widget_class, on_mouse_target_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_send_disposition);

  gobject_class->dispose = stamp_message_list_item_dispose;

  gobject_class->get_property = stamp_message_list_item_get_property;
  gobject_class->set_property = stamp_message_list_item_set_property;

  properties[PROP_ACCOUNT] =
    g_param_spec_object ("account",
                         NULL,
                         NULL,
                         STAMP_TYPE_ACCOUNT,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_MESSAGE_INFO] =
    g_param_spec_object ("message-info",
                         NULL,
                         NULL,
                         CAMEL_TYPE_MESSAGE_INFO,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_EXPANDED] =
    g_param_spec_boolean ("expanded",
                          NULL,
                          NULL,
                          TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (properties), properties);
}

static void
on_size_request (GtkWidget  *web_view,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  gint width;
  gint height;
  gint my_width;
  GtkWidget *list;
  gboolean mobile;

  stamp_web_view_get_size (self->web_view, &width, &height);

  my_width = gtk_widget_get_width (GTK_WIDGET (self));

  list = gtk_widget_get_ancestor (GTK_WIDGET (self), STAMP_TYPE_MESSAGE_LIST);
  mobile = list && stamp_message_list_get_mobile_mode (STAMP_MESSAGE_LIST (list));

  if (mobile && width > my_width) {
    gdouble zoom_level = (double)my_width / (double)width;

    webkit_web_view_set_zoom_level (WEBKIT_WEB_VIEW (self->web_view), zoom_level);
    gtk_widget_set_size_request (GTK_WIDGET (self->web_view), -1, height * zoom_level);
  } else {
    webkit_web_view_set_zoom_level (WEBKIT_WEB_VIEW (self->web_view), 1);
    gtk_widget_set_size_request (GTK_WIDGET (self->web_view), -1, height);
  }
}

static void
on_mark_read_activate (GSimpleAction *action,
                       GVariant      *parameter,
                       gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  camel_message_info_set_flags ((CamelMessageInfo *)self->message_info, CAMEL_MESSAGE_SEEN, ~0);
  update_actions (self);
}

static void
on_mark_unread_activate (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  camel_message_info_set_flags ((CamelMessageInfo *)self->message_info, CAMEL_MESSAGE_SEEN, 0);
  update_actions (self);
}

static void
on_mark_flag_activate (GSimpleAction *action,
                       GVariant      *parameter,
                       gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  camel_message_info_set_flags ((CamelMessageInfo *)self->message_info, CAMEL_MESSAGE_FLAGGED, ~0);
  update_actions (self);
}

static void
on_mark_unflag_activate (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  camel_message_info_set_flags ((CamelMessageInfo *)self->message_info, CAMEL_MESSAGE_FLAGGED, 0);
  update_actions (self);
}

static void
on_print (GSimpleAction *action,
          GVariant      *parameter,
          gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  stamp_message_list_item_print (self);
}

static void
on_view_source_activate (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  stamp_message_list_item_view_source (self);
}

static void
on_message_body (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  GtkWidget *composer;
  g_autoptr (GError) error = NULL;
  gchar *body;

  body = stamp_message_list_item_get_message_body_html_finish (self, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Could not get message body html: %s", error->message);
    return;
  }

  if (!body) {
    g_warning ("Could not get message body html: empty response");
    return;
  }

  composer = stamp_composer_new_with_quote (self->type,
                                            stamp_message_list_item_get_uid (self),
                                            self->account,
                                            stamp_message_list_item_get_web_view (self),
                                            stamp_message_list_item_get_message_info (self),
                                            stamp_message_list_item_get_message (self),
                                            body);
  gtk_window_present (GTK_WINDOW (composer));
}

static void
stamp_message_list_item_compose (StampMessageListItem *self,
                                 StampComposerType     type)
{
  if (type == STAMP_COMPOSER_NEW) {
    GtkWidget *composer;

    composer = stamp_composer_new (self->account);
    gtk_window_present (GTK_WINDOW (composer));
    return;
  }

  self->type = type;
  stamp_message_list_item_get_message_body_html (self, NULL, on_message_body, self);
}

static void
on_reply (GSimpleAction *action,
          GVariant      *parameter,
          gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  stamp_message_list_item_compose (self, STAMP_COMPOSER_REPLY);
}

static void
on_reply_all (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  stamp_message_list_item_compose (self, STAMP_COMPOSER_REPLY_ALL);
}

static void
on_forward (GSimpleAction *action,
            GVariant      *parameter,
            gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  stamp_message_list_item_compose (self, STAMP_COMPOSER_FORWARD);
}


static void
on_unsubscribe_response (GtkWidget *dialog,
                         gchar     *response,
                         gpointer   user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  if (g_strcmp0 (response, "confirm") == 0) {
    g_autoptr (StampMimeParser) parser = NULL;

    parser = stamp_mime_parser_new (CAMEL_SESSION (stamp_session_get_default ()));
    stamp_mime_parser_parse (parser, self->message, self->cancellable, NULL);

    if (self->unsubscribe->method == STAMP_MIME_UNSUBSCRIBE_HTTP) {
      stamp_mime_parser_send_unsubscribe (parser, self->unsubscribe, self->cancellable);
    } else {
      GtkWidget *composer = stamp_composer_new (self->account);

      stamp_composer_set_to (STAMP_COMPOSER (composer), self->unsubscribe->mailto);
      gtk_window_present (GTK_WINDOW (composer));
    }
  }
}

static void
on_unsubscribe_activate (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  AdwDialog *dialog;
  g_autofree char *body = NULL;

  if (!self->unsubscribe)
    return;

  body = g_strdup_printf (_("Are you sure you want to unsubscribe from the mailing list?\n\nUnsubscribe URL: %s"), self->unsubscribe->uri);

  dialog = adw_alert_dialog_new (_("Unsubscribe"), body);

  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "confirm", _("Unsubscribe"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "confirm", ADW_RESPONSE_DESTRUCTIVE);

  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_unsubscribe_response), self);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static const GActionEntry actions[] = {
  { "reply", on_reply},
  { "reply-all", on_reply_all},
  { "forward", on_forward},
  { "mark-read", on_mark_read_activate},
  { "mark-unread", on_mark_unread_activate},
  { "mark-flag", on_mark_flag_activate},
  { "mark-unflag", on_mark_unflag_activate},
  { "unsubscribe", on_unsubscribe_activate},
  { "print", on_print},
  { "view-source", on_view_source_activate},
};

static GInputStream *
on_cid_request (WebKitURISchemeRequest *request,
                gpointer                user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  g_autofree char *path = g_uri_unescape_string (webkit_uri_scheme_request_get_path (request), NULL);
  GList *attachments;

  attachments = stamp_mime_parser_get_inline_images (self->parser);
  for (GList *iter = attachments; iter && iter->data; iter = g_list_next (iter)) {
    StampMimeAttachment *att = iter->data;

    if (g_strcmp0 (att->content_id, path) == 0) {
      GInputStream *stream = g_memory_input_stream_new_from_bytes (att->data);
      return stream;
    }
  }

  /* Mail clients are stupid and often declare inline images as attachments… */
  attachments = stamp_mime_parser_get_attachments (self->parser);
  for (GList *iter = attachments; iter && iter->data; iter = g_list_next (iter)) {
    StampMimeAttachment *att = iter->data;

    if (g_strcmp0 (att->content_id, path) == 0) {
      GInputStream *stream = g_memory_input_stream_new_from_bytes (att->data);
      return stream;
    }
  }

  return NULL;
}

static void
on_save_as (GObject      *source_object,
            GAsyncResult *res,
            gpointer      user_data)
{
  StampMimeAttachment *att = user_data;
  g_autoptr (GtkFileDialog) dialog = GTK_FILE_DIALOG (source_object);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GFileOutputStream) file_output_stream = NULL;
  g_autoptr (GFileIOStream) file_io_stream = NULL;
  GOutputStream *stream = NULL;

  file = gtk_file_dialog_save_finish (dialog, res, &error);
  if (error) {
    g_warning ("%s: Could not save attachment file: %s", G_STRFUNC, error->message);
    return;
  }

  file_output_stream = g_file_create (file, G_FILE_CREATE_NONE, NULL, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
      g_warning ("%s: Could not create file: %s", G_STRFUNC, error->message);
      return;
    }

    g_clear_error (&error);
    file_io_stream = g_file_open_readwrite (file, NULL, &error);
    if (error) {
      g_warning ("%s: Could not open file: %s", G_STRFUNC, error->message);
      return;
    }

    stream = g_io_stream_get_output_stream (G_IO_STREAM (file_io_stream));
  } else {
    stream = G_OUTPUT_STREAM (file_output_stream);
  }

  g_output_stream_write_all (stream, g_bytes_get_data (att->data, NULL), g_bytes_get_size (att->data), NULL, NULL, &error);
  if (error) {
    g_warning ("%s: Could not write file, abort: %s", G_STRFUNC, error->message);
    return;
  }
}

static void
save_image_cb (GSimpleAction *action,
               GVariant      *parameter,
               gpointer       user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);
  GList *attachments;
  gint offset = 0;

  if (g_str_has_prefix (self->save_img, "cid:"))
    offset += 4;

  attachments = stamp_mime_parser_get_inline_images (self->parser);
  for (GList *iter = attachments; iter && iter->data; iter = g_list_next (iter)) {
    StampMimeAttachment *att = iter->data;

    if (g_strcmp0 (att->content_id, self->save_img + offset) == 0) {
      GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
      GtkFileDialog *dialog = gtk_file_dialog_new ();
      g_autofree char *tmp = g_strdup_printf (_("Save `%s`?"), att->filename);

      gtk_file_dialog_set_accept_label (dialog, _("Save"));
      gtk_file_dialog_set_initial_name (dialog, att->filename);
      gtk_file_dialog_set_title (dialog, tmp);

      gtk_file_dialog_save (dialog, GTK_WINDOW (root), NULL, on_save_as, att);
      return;
    }
  }
}

static gboolean
on_context_menu (WebKitWebView       *web_view,
                 WebKitContextMenu   *context_menu,
                 WebKitHitTestResult *hit_test_result,
                 gpointer             user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  if (webkit_hit_test_result_context_is_image (hit_test_result)) {
    GSimpleAction *action;
    WebKitContextMenuItem *item;
    const gchar *image_uri = webkit_hit_test_result_get_image_uri (hit_test_result);

    webkit_context_menu_remove_all (context_menu);

    action = g_simple_action_new ("save-image", NULL);
    g_set_str (&self->save_img, image_uri);
    g_signal_connect (action, "activate", G_CALLBACK (save_image_cb), self);

    item = webkit_context_menu_item_new_from_gaction (G_ACTION (action), _("Save Image As…"), NULL);
    webkit_context_menu_append (context_menu, item);
  }

  return FALSE;
}

void
stamp_message_list_item_init (StampMessageListItem *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->message = NULL;
  self->message_loaded = FALSE;

  self->cancellable = g_cancellable_new ();

  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions), actions, G_N_ELEMENTS (actions), self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "message-list-item", G_ACTION_GROUP (self->actions));

  g_signal_connect (self->save_all_button, "clicked", G_CALLBACK (on_save_all), self);
}

GtkWidget *
stamp_message_list_item_new (StampAccount          *account,
                             CamelFolderThreadNode *thread_node)
{
  const CamelMessageInfo *message = camel_folder_thread_node_get_item (thread_node);
  StampMessageListItem *ret = g_object_new (STAMP_TYPE_MESSAGE_LIST_ITEM,
                                            "account", account,
                                            "message-info", message,
                                            NULL);

  stamp_message_header_set_mail (STAMP_MESSAGE_HEADER (ret->header), thread_node);
  update_actions (ret);

  return GTK_WIDGET (ret);
}

const CamelMessageInfo *
stamp_message_list_item_get_message_info (StampMessageListItem *self)
{
  return self->message_info;
}

void
stamp_message_list_item_get_message_body_html (StampMessageListItem *self,
                                               GCancellable         *cancellable,
                                               GAsyncReadyCallback   callback,
                                               gpointer              user_data)
{
  ensure_web_view (self);
  stamp_webview_get_body_html (self->web_view, cancellable, callback, user_data);
}

gchar *
stamp_message_list_item_get_message_body_html_finish (StampMessageListItem  *self,
                                                      GAsyncResult          *res,
                                                      GError               **error)
{
  return stamp_webview_get_body_html_finish (self->web_view, res, NULL, error);
}

CamelMimeMessage *
stamp_message_list_item_get_message (StampMessageListItem *self)
{
  return self->message;
}

guint64
stamp_message_list_item_get_timestamp (StampMessageListItem *self)
{
  guint64 timestamp = camel_message_info_get_date_received (self->message_info);

  if (!timestamp)
    timestamp = camel_message_info_get_date_sent (self->message_info);

  return timestamp;
}

void
stamp_message_list_item_print (StampMessageListItem *self)
{
  GtkPrintSettings *settings = gtk_print_settings_new ();
  WebKitPrintOperation *operation;
  const gchar *subject = camel_message_info_get_subject (self->message_info);
  g_autofree char *filename = g_strdup (_("E-Mail Message"));
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  if (subject && strlen (subject) > 0) {
    GRegex *regex = g_regex_new ("[[:space:][:cntrl:]/]+", 0, 0, NULL);
    g_autofree char *new_string = NULL;

    new_string = g_regex_replace (regex, subject, (gssize) - 1, 0, " ", 0, NULL);
    if (strlen (new_string) < 64) {
      g_set_str (&filename, new_string);
    } else {
      filename = g_strdup_printf ("%60s", new_string);
    }
  }

  gtk_print_settings_set (settings, GTK_PRINT_SETTINGS_OUTPUT_BASENAME, filename);
  if (!self->web_view)
    return;

  operation = webkit_print_operation_new (WEBKIT_WEB_VIEW (self->web_view));

  webkit_print_operation_set_print_settings (operation, settings);
  webkit_print_operation_run_dialog (operation, GTK_WINDOW (root));
}

const gchar *
stamp_message_list_item_get_uid (StampMessageListItem *self)
{
  return camel_message_info_get_uid (self->message_info);
}

#define VIEW_SOURCE_TMP_DIR "/tmp/stamp-view-source"

static void
cleanup_view_source_dir (void)
{
  GDir *dir = g_dir_open (VIEW_SOURCE_TMP_DIR, 0, NULL);
  const gchar *name;

  if (!dir)
    return;

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (VIEW_SOURCE_TMP_DIR, name, NULL);
    g_remove (child);
  }

  g_dir_close (dir);
  g_remove (VIEW_SOURCE_TMP_DIR);
}

void
stamp_message_list_item_view_source (StampMessageListItem *self)
{
  static gboolean initialized = FALSE;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (CamelStream) stream = NULL;
  g_autofree char *tmpl = NULL;
  GByteArray *array = NULL;
  gint fd;

  if (!initialized) {
    initialized = TRUE;

    cleanup_view_source_dir ();

    if (g_mkdir (VIEW_SOURCE_TMP_DIR, 0700) != 0) {
      g_warning ("%s: Could not create temp directory %s", G_STRFUNC, VIEW_SOURCE_TMP_DIR);
      return;
    }

    atexit (cleanup_view_source_dir);
  }

  tmpl = g_build_filename (VIEW_SOURCE_TMP_DIR, "stamp-XXXXXX", NULL);
  fd = g_mkstemp (tmpl);
  if (fd == -1) {
    g_warning ("%s: Temp file failed: %s", G_STRFUNC, g_strerror (errno));
    return;
  }

  path = g_strdup_printf ("%s.txt", tmpl);
  g_rename (tmpl, path);

  array = g_byte_array_new ();
  stream = camel_stream_mem_new_with_byte_array (array);
  camel_data_wrapper_write_to_stream_sync (CAMEL_DATA_WRAPPER (self->message), stream, self->cancellable, &error);

  write (fd, (char *)array->data, array->len);
  close (fd);

  file = g_file_new_for_path (path);
  if (!g_app_info_launch_default_for_uri (g_file_get_uri (file), NULL, &error))
    g_warning ("%s: Launch failed: %s", G_STRFUNC, error->message);
}

static void
finish_search (StampMessageListItem *self,
               gboolean              found)
{
  StampMessageListItemSearchDone done = self->search_done;
  gpointer user_data = self->search_done_data;

  self->search_done = NULL;
  self->search_done_data = NULL;

  if (found)
    stamp_message_list_item_set_expanded (self, TRUE);

  if (done)
    done (self, found, user_data);
}

static void
on_found_text (WebKitFindController *controller,
               guint                 match_count,
               gpointer              user_data)
{
  finish_search (STAMP_MESSAGE_LIST_ITEM (user_data), match_count > 0);
}

static void
on_failed_to_find_text (WebKitFindController *controller,
                        gpointer              user_data)
{
  finish_search (STAMP_MESSAGE_LIST_ITEM (user_data), FALSE);
}

static void
run_pending_search (StampMessageListItem *self)
{
  WebKitFindController *controller;

  if (!self->pending_search || !self->web_view || !self->web_view_loaded)
    return;

  controller = webkit_web_view_get_find_controller (WEBKIT_WEB_VIEW (self->web_view));

  if (!self->find_connected) {
    self->find_connected = TRUE;
    g_signal_connect (controller, "found-text", G_CALLBACK (on_found_text), self);
    g_signal_connect (controller, "failed-to-find-text", G_CALLBACK (on_failed_to_find_text), self);
  }

  webkit_find_controller_search (controller, self->pending_search, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE, G_MAXUINT);
  g_clear_pointer (&self->pending_search, g_free);
}

static void
on_web_view_loaded (StampWebView *web_view,
                    gboolean      finished,
                    gpointer      user_data)
{
  StampMessageListItem *self = STAMP_MESSAGE_LIST_ITEM (user_data);

  if (!finished)
    return;

  self->web_view_loaded = TRUE;
  run_pending_search (self);
}

static void
ensure_web_view (StampMessageListItem *self)
{
  if (self->web_view)
    return;

  self->web_view = stamp_webview_new ();
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->web_view), 2);
  gtk_widget_set_size_request (GTK_WIDGET (self->web_view), 1, 1);

  g_signal_connect (self->web_view, "image-load-blocked", G_CALLBACK (on_image_load_blocked), self);
  g_signal_connect (self->web_view, "mouse-target-changed", G_CALLBACK (on_mouse_target_changed), self);
  g_signal_connect_object (self->web_view, "notify::size-request", G_CALLBACK (on_size_request), self, G_CONNECT_DEFAULT);
  g_signal_connect (self->web_view, "loaded", G_CALLBACK (on_web_view_loaded), self);
  stamp_webview_set_cid_handler (self->web_view, on_cid_request, self);
  g_signal_connect (self->web_view, "context-menu", G_CALLBACK (on_context_menu), self);

  gtk_box_append (self->web_view_box, GTK_WIDGET (self->web_view));
}

void
stamp_message_list_item_search (StampMessageListItem           *self,
                                const gchar                    *search_text,
                                StampMessageListItemSearchDone  done,
                                gpointer                        user_data)
{
  g_free (self->pending_search);
  self->pending_search = g_strdup (search_text);
  self->search_done = done;
  self->search_done_data = user_data;

  if (!self->message_loaded) {
    start_get_message (self, NULL, NULL);
    self->message_loaded = TRUE;
  }

  run_pending_search (self);
}

StampWebView *
stamp_message_list_item_get_web_view (StampMessageListItem *self)
{
  return self->web_view;
}
