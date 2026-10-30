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

#include "stamp-contact-details.h"

#include <adwaita.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#include "stamp-account.h"
#include "stamp-session.h"

struct _StampContactDetails {
  GtkBox parent_instance;

  GtkWidget *avatar;
  GtkWidget *name;
  GtkWidget *stack;

  GtkWidget *mail;
  GtkWidget *phone;
  GtkWidget *birthday;
  GtkWidget *address;
  GtkWidget *copy_button;
  GtkWidget *toast_overlay;

  EContact *contact;
  EClient *source_client;
  gboolean copying;

  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (StampContactDetails, stamp_contact_details, GTK_TYPE_BOX);

typedef struct {
  EContact *contact;
  GPtrArray *targets;
  AdwComboRow *row;
} CopySelection;

static void
copy_selection_free (gpointer data)
{
  CopySelection *selection = data;

  g_object_unref (selection->contact);
  g_ptr_array_unref (selection->targets);
  g_free (selection);
}

typedef struct {
  StampContactDetails *self;
  gchar *target_name;
} CopyRequest;

static void
show_copy_message (StampContactDetails *self,
                   const gchar         *title,
                   const gchar         *body)
{
  AdwDialog *dialog;

  if (!gtk_widget_get_root (GTK_WIDGET (self)))
    return;

  dialog = adw_alert_dialog_new (title, body);

  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("Close"));
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "close");
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_contact_copied (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  CopyRequest *request = user_data;
  StampContactDetails *self = request->self;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *uid = NULL;

  self->copying = FALSE;
  gtk_widget_set_sensitive (self->copy_button, self->contact != NULL);

  if (e_book_client_add_contact_finish (E_BOOK_CLIENT (source), result, &uid, &error)) {
    g_autofree gchar *message = g_strdup_printf (_("Contact copied to %s"), request->target_name);
    AdwToast *toast = adw_toast_new (message);

    adw_toast_set_use_markup (toast, FALSE);
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
  } else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    show_copy_message (self, _("Could Not Copy Contact"), error->message);
  }

  g_object_unref (self);
  g_free (request->target_name);
  g_free (request);
}

static void
on_copy_response (AdwAlertDialog *dialog,
                  const gchar    *response,
                  gpointer        user_data)
{
  StampContactDetails *self = STAMP_CONTACT_DETAILS (user_data);
  CopySelection *selection = g_object_get_data (G_OBJECT (dialog), "copy-selection");
  EBookClient *target;
  guint selected;
  CopyRequest *request;

  if (!g_str_equal (response, "copy") || self->copying)
    return;

  selected = adw_combo_row_get_selected (selection->row);
  if (selected >= selection->targets->len)
    return;

  target = g_ptr_array_index (selection->targets, selected);
  if (e_client_is_readonly (E_CLIENT (target)) ||
      !e_source_get_enabled (e_client_get_source (E_CLIENT (target)))) {
    show_copy_message (self, _("Could Not Copy Contact"), _("The selected address book is no longer writable."));
    return;
  }

  request = g_new0 (CopyRequest, 1);
  request->self = g_object_ref (self);
  request->target_name = g_strdup (e_source_get_display_name (e_client_get_source (E_CLIENT (target))));
  self->copying = TRUE;
  gtk_widget_set_sensitive (self->copy_button, FALSE);
  e_book_client_add_contact (target, selection->contact, E_BOOK_OPERATION_FLAG_NONE,
                             self->cancellable, on_contact_copied, request);
}

static EContact *
contact_duplicate_for_copy (EContact *contact)
{
  EContact *copy = e_contact_duplicate (contact);
  const gchar *metadata[] = {
    "X-EWS-ORIGINAL-VCARD", "X-EWS-CHANGEKEY", "X-EWS-GAL-SHA1", "X-EWS-PHOTO-CHECK-DATE",
  };

  /* The destination creates its own identity and server-side revision. */
  e_contact_set (copy, E_CONTACT_UID, NULL);
  e_contact_set (copy, E_CONTACT_REV, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (metadata); i++)
    e_vcard_remove_attributes (E_VCARD (copy), NULL, metadata[i]);

  return copy;
}

static void
on_copy_contact (GtkButton *button,
                 gpointer   user_data)
{
  StampContactDetails *self = STAMP_CONTACT_DETAILS (user_data);
  CopySelection *selection;
  g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
  GList *accounts = stamp_session_get_accounts (stamp_session_get_default ());
  AdwDialog *dialog;
  GtkWidget *list;
  const gchar *source_uid;

  if (!self->contact || !self->source_client || self->copying)
    return;

  selection = g_new0 (CopySelection, 1);
  selection->contact = contact_duplicate_for_copy (self->contact);
  selection->targets = g_ptr_array_new_with_free_func (g_object_unref);
  source_uid = e_source_get_uid (e_client_get_source (self->source_client));

  for (GList *iter = accounts; iter; iter = iter->next) {
    StampAccount *account = iter->data;
    GPtrArray *books = stamp_account_get_books (account);

    for (guint i = 0; books && i < books->len; i++) {
      StampContactsService *service = g_ptr_array_index (books, i);
      EBookClient *client = stamp_contacts_service_get_client (service);
      ESource *source;
      g_autofree gchar *label = NULL;

      if (!stamp_contacts_service_get_enabled (service) || !client || e_client_is_readonly (E_CLIENT (client)))
        continue;

      source = e_client_get_source (E_CLIENT (client));
      if (!e_source_get_enabled (source) || g_strcmp0 (source_uid, e_source_get_uid (source)) == 0)
        continue;

      label = g_strdup_printf ("%s — %s", stamp_account_get_name (account), e_source_get_display_name (source));
      gtk_string_list_append (names, label);
      g_ptr_array_add (selection->targets, g_object_ref (client));
    }
  }

  if (selection->targets->len == 0) {
    copy_selection_free (selection);
    show_copy_message (self, _("No Writable Address Book"), _("There is no other writable address book available."));
    return;
  }

  dialog = adw_alert_dialog_new (_("Copy to Address Book"), _("Choose where to copy this contact."));
  list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  selection->row = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (selection->row), _("Address Book"));
  adw_combo_row_set_use_subtitle (selection->row, TRUE);
  adw_combo_row_set_model (selection->row, G_LIST_MODEL (names));
  gtk_list_box_append (GTK_LIST_BOX (list), GTK_WIDGET (selection->row));
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), list);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel", _("Cancel"), "copy", _("Copy"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "copy", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "copy");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_object_set_data_full (G_OBJECT (dialog), "copy-selection", selection, copy_selection_free);
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_copy_response), self, G_CONNECT_DEFAULT);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
stamp_contact_details_init (StampContactDetails *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->cancellable = g_cancellable_new ();
}

static void
stamp_contact_details_dispose (GObject *object)
{
  StampContactDetails *self = STAMP_CONTACT_DETAILS (object);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->contact);
  g_clear_object (&self->source_client);

  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_CONTACT_DETAILS);

  G_OBJECT_CLASS (stamp_contact_details_parent_class)->dispose (object);
}

static void
stamp_contact_details_class_init (StampContactDetailsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/contact/stamp-contact-details.ui");

  object_class->dispose = stamp_contact_details_dispose;

  gtk_widget_class_bind_template_child (widget_class, StampContactDetails, stack);
  gtk_widget_class_bind_template_child (widget_class, StampContactDetails, avatar);
  gtk_widget_class_bind_template_child (widget_class, StampContactDetails, name);
  gtk_widget_class_bind_template_child (widget_class, StampContactDetails, mail);
  gtk_widget_class_bind_template_child (widget_class, StampContactDetails, phone);
  gtk_widget_class_bind_template_child (widget_class, StampContactDetails, birthday);
  gtk_widget_class_bind_template_child (widget_class, StampContactDetails, address);
  gtk_widget_class_bind_template_child (widget_class, StampContactDetails, copy_button);
  gtk_widget_class_bind_template_child (widget_class, StampContactDetails, toast_overlay);
  gtk_widget_class_bind_template_callback (widget_class, on_copy_contact);
}

static void
add_mail_row (GtkWidget *list_box,
              gchar     *mail)
{
  GtkWidget *row;
  g_autofree char *markup = g_markup_escape_text (mail, -1);
  g_autofree char *url = g_strdup_printf ("mailto:%s", markup);
  GtkWidget *mailto_button = gtk_link_button_new (url);

  row = adw_action_row_new ();
  adw_preferences_row_set_title_selectable (ADW_PREFERENCES_ROW (row), TRUE);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("mail-unread-symbolic"));

  gtk_widget_set_tooltip_text (mailto_button, url);
  gtk_button_set_child (GTK_BUTTON (mailto_button), gtk_image_new_from_icon_name ("mail-send-symbolic"));
  gtk_widget_set_tooltip_text (mailto_button, _("Send mail"));
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), mailto_button);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), markup);

  gtk_list_box_append (GTK_LIST_BOX (list_box), row);
}

static void
add_phone_row (GtkWidget   *list_box,
               const gchar *type,
               const gchar *number)
{
  GtkWidget *row;
  g_autoptr (GError) error = NULL;
  g_autofree char *region = e_phone_number_get_default_region (&error);
  g_autofree char *formatted_number = NULL;
  g_autofree char *url = NULL;
  GtkWidget *tel_button;

  if (region) {
    EPhoneNumber *phone_number;

    phone_number = e_phone_number_from_string (number, region, &error);
    if (!error)
      formatted_number = g_strdup (e_phone_number_to_string (phone_number, E_PHONE_NUMBER_FORMAT_INTERNATIONAL));
  }

  if (!formatted_number)
    formatted_number = g_strdup (number);

  url = g_strdup_printf ("tel:%s", formatted_number);

  tel_button = gtk_link_button_new (url);
  row = adw_action_row_new ();
  adw_preferences_row_set_title_selectable (ADW_PREFERENCES_ROW (row), TRUE);

  adw_action_row_add_prefix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("call-start-symbolic"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), formatted_number);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), type);
  gtk_button_set_child (GTK_BUTTON (tel_button), gtk_image_new_from_icon_name ("call-outgoing-symbolic"));
  gtk_widget_set_tooltip_text (tel_button, _("Start call"));
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), tel_button);
  gtk_list_box_append (GTK_LIST_BOX (list_box), row);
}

static struct {
  EContactField field;
  const char *name;
} phones[] = {
  { E_CONTACT_PHONE_MOBILE, N_("Mobile") },
  { E_CONTACT_PHONE_BUSINESS, N_("Business") },
  { E_CONTACT_PHONE_HOME, N_("Home") },
  { E_CONTACT_PHONE_PRIMARY, N_("Primary") },
  { E_CONTACT_PHONE_OTHER, N_("Other") },

/* E_CONTACT_PHONE_ASSISTANT, */
/* E_CONTACT_PHONE_BUSINESS_2, */
/* E_CONTACT_PHONE_BUSINESS_FAX, */
/* E_CONTACT_PHONE_CALLBACK, */
/* E_CONTACT_PHONE_CAR, */
/* E_CONTACT_PHONE_COMPANY, */
/* E_CONTACT_PHONE_HOME_2, */
/* E_CONTACT_PHONE_HOME_FAX, */
/* E_CONTACT_PHONE_ISDN, */
/* E_CONTACT_PHONE_OTHER_FAX, */
/* E_CONTACT_PHONE_PAGER, */
/* E_CONTACT_PHONE_RADIO, */
/* E_CONTACT_PHONE_TELEX, */
/* E_CONTACT_PHONE_TTYTDD, */
};

static struct {
  EContactField field;
  const char *name;
  const char *icon;
} org[] = {
  { E_CONTACT_ORG, N_("Company"), "building-symbolic" },
  { E_CONTACT_ORG_UNIT, N_("Unit"), "building-symbolic" },
  { E_CONTACT_OFFICE, N_("Office"), "mark-location-symbolic" },
};

static void
add_org_row (GtkWidget   *list_box,
             const gchar *icon,
             const gchar *type,
             const gchar *text)
{
  GtkWidget *row;
  gchar *markup = g_markup_escape_text (text, -1);

  row = adw_action_row_new ();
  adw_preferences_row_set_title_selectable (ADW_PREFERENCES_ROW (row), TRUE);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name (icon));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), markup);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), type);
  gtk_list_box_append (GTK_LIST_BOX (list_box), row);
}

void
stamp_contact_details_show (StampContactDetails *self,
                            StampAccount        *account,
                            EClient             *client,
                            StampContactItem    *contact)
{
  g_autofree char *mail = NULL;
  g_autofree char *name = NULL;
  g_autoptr (GdkTexture) photo = NULL;
  g_autoptr (EContactDate) birthdate = NULL;
  g_autoptr (EContactAddress) address = NULL;
  const gchar *item_mail;
  EContact *e_contact = contact ? stamp_contact_item_get_contact (contact) : NULL;
  GList *mails;
  gboolean mail_added = FALSE;
  gboolean phone_added = FALSE;
  gboolean org_added = FALSE;

  g_set_object (&self->source_client, client);
  g_set_object (&self->contact, e_contact);
  gtk_widget_set_sensitive (self->copy_button, self->contact != NULL && !self->copying);

  if (!contact) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
    return;
  }

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "content");
  item_mail = stamp_contact_item_get_mail (contact);
  if (item_mail)
    mail = g_utf8_strdown (item_mail, -1);

  name = g_strdup (stamp_contact_item_get_name (contact));
  if (!name || strlen (name) == 0)
    name = g_strdup (mail);

  adw_avatar_set_text (ADW_AVATAR (self->avatar), name);

  photo = stamp_contact_item_get_photo (contact);
  adw_avatar_set_custom_image (ADW_AVATAR (self->avatar), photo ? GDK_PAINTABLE (photo) : NULL);
  e_contact = stamp_contact_item_get_contact (contact);

  gtk_label_set_text (GTK_LABEL (self->name), name);

  /* Mail */
  gtk_list_box_remove_all (GTK_LIST_BOX (self->mail));

  mails = e_contact_get (e_contact, E_CONTACT_EMAIL);
  for (GList *iter = mails; iter && iter->data; iter = g_list_next (iter)) {
    g_autofree char *tmp = g_utf8_strdown (iter->data, -1);
    add_mail_row (self->mail, tmp);
    mail_added = TRUE;
  }
  g_list_free_full (mails, g_free);

  gtk_widget_set_visible (self->mail, mail_added);

  /* Phone */
  gtk_list_box_remove_all (GTK_LIST_BOX (self->phone));

  for (gint idx = 0; idx < G_N_ELEMENTS (phones); idx++) {
    const gchar *number = e_contact_get_const (e_contact, phones[idx].field);

    if (number && strlen (number) > 0) {
      add_phone_row (self->phone, phones[idx].name, number);
      phone_added = TRUE;
    }
  }

  gtk_widget_set_visible (self->phone, phone_added);

  /* Birthday */
  gtk_list_box_remove_all (GTK_LIST_BOX (self->birthday));
  birthdate = e_contact_get (e_contact, E_CONTACT_BIRTH_DATE);
  if (birthdate) {
    GtkWidget *row;
    g_autoptr (GDateTime) date = NULL;
    g_autofree char *str = NULL;

    date = g_date_time_new_local (birthdate->year,
                                  birthdate->month,
                                  birthdate->day,
                                  0, 0, 0.0);

    str = g_date_time_format (date, "%d.%m.%Y");

    row = adw_action_row_new ();
    adw_action_row_add_prefix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("birthday-symbolic"));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), str);
    gtk_list_box_append (GTK_LIST_BOX (self->birthday), row);

    gtk_widget_set_visible (self->birthday, TRUE);
  }
  gtk_widget_set_visible (self->birthday, birthdate != NULL);

  /* Org */
  gtk_list_box_remove_all (GTK_LIST_BOX (self->address));

  for (gint idx = 0; idx < G_N_ELEMENTS (org); idx++) {
    const gchar *text = e_contact_get_const (e_contact, org[idx].field);

    if (text && strlen (text) > 0) {
      add_org_row (self->address, org[idx].icon, org[idx].name, text);
      org_added = TRUE;
    }
  }

  address = e_contact_get (e_contact, E_CONTACT_ADDRESS_HOME);
  if (address) {
    GtkWidget *row;
    g_autoptr (GString) str = g_string_new (NULL);

    if (address->street)
      g_string_append_printf (str, "%s\n", address->street);

    if (address->code || address->locality) {
      if (address->code)
        g_string_append_printf (str, "%s", address->code);

      if (address->locality)
        g_string_append_printf (str, " %s", address->locality);

      g_string_append (str, "\n");
    }

    if (address->country)
      g_string_append_printf (str, "%s", address->country);

    row = adw_action_row_new ();
    adw_action_row_add_prefix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("mark-location-symbolic"));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), g_strstrip (str->str));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _("Home"));
    gtk_list_box_append (GTK_LIST_BOX (self->address), row);

    org_added = TRUE;
  }

  address = e_contact_get (e_contact, E_CONTACT_ADDRESS_WORK);
  if (address) {
    GtkWidget *row;
    g_autoptr (GString) str = g_string_new (NULL);

    if (address->street)
      g_string_append_printf (str, "%s\n", address->street);

    if (address->code || address->locality) {
      if (address->code)
        g_string_append_printf (str, "%s", address->code);

      if (address->locality)
        g_string_append_printf (str, " %s", address->locality);

      g_string_append (str, "\n");
    }

    if (address->country)
      g_string_append_printf (str, "%s", address->country);

    row = adw_action_row_new ();
    adw_action_row_add_prefix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("mark-location-symbolic"));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), g_strstrip (str->str));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _("Work"));
    gtk_list_box_append (GTK_LIST_BOX (self->address), row);

    org_added = TRUE;
  }


  gtk_widget_set_visible (self->address, org_added);
}
