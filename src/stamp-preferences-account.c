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

#include "stamp-preferences-account.h"

#include <locale.h>

#include <camel/camel.h>
#include <cert.h>
#include <glib/gi18n.h>
#include <gpgme.h>
#include <nss.h>
#include <pk11pub.h>

#include "stamp-account.h"
#include "stamp-helper.h"
#include "stamp-out-of-office.h"
#include "stamp-preferences-out-of-office.h"
#include "stamp-session.h"
#include "stamp-settings.h"
#include "stamp-signature.h"

struct _StampPreferencesAccount {
  AdwNavigationPage parent_instance;

  AdwWindowTitle *window_title;
  AdwPreferencesPage *page;
  AdwComboRow *default_signature;
  GtkFlowBox *badge_color_box;
  GtkButton *reset_badge_color_button;
  gboolean updating_badge_colors;
  AdwPreferencesGroup *alias_group;
  AdwComboRow *notification_mode;
  AdwPreferencesGroup *notification_folders_group;

  AdwComboRow *pgp_key;
  AdwComboRow *smime_sign_cert;
  AdwComboRow *smime_encrypt_cert;

  GPtrArray *pgp_key_ids;
  GPtrArray *smime_cert_nicknames;

  AdwPreferencesGroup *out_of_office_group;
  StampAccount *account;
  GHashTable *aliases;
  GPtrArray *alias_rows;

  GSettings *account_settings;
  GPtrArray *folder_rows;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (StampPreferencesAccount, stamp_preferences_account, ADW_TYPE_NAVIGATION_PAGE);

typedef enum {
  PROP_ACCOUNT = 1,
} StampPreferencesAccountProps;

static GParamSpec *props[PROP_ACCOUNT + 1] = { NULL, };

static void
on_alias_edit_clicked (GtkWidget *button,
                       gpointer   user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  AdwActionRow *row = ADW_ACTION_ROW (g_object_get_data (G_OBJECT (button), "row"));
  const gchar *mail = adw_preferences_row_get_title (ADW_PREFERENCES_ROW (row));
  const gchar *name = adw_action_row_get_subtitle (row);
  AdwNavigationPage *page;
  GtkWidget *parent;

  parent = gtk_widget_get_parent (GTK_WIDGET (self));
  page = ADW_NAVIGATION_PAGE (stamp_preferences_account_editor_new (self->account, mail, name));
  adw_navigation_view_push (ADW_NAVIGATION_VIEW (parent), page);
}

static void
on_alias_activated (AdwActionRow *row,
                    gpointer      user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  const gchar *mail = g_object_get_data (G_OBJECT (row), "email");
  const gchar *name = g_object_get_data (G_OBJECT (row), "name");
  AdwNavigationPage *page;
  GtkWidget *parent;

  parent = gtk_widget_get_parent (GTK_WIDGET (self));
  page = ADW_NAVIGATION_PAGE (stamp_preferences_account_editor_new (self->account, mail, name));
  adw_navigation_view_push (ADW_NAVIGATION_VIEW (parent), page);
}

static void
refresh_alias_list (StampPreferencesAccount *self)
{
  StampMailService *service;
  GList *mails;

  if (self->alias_rows) {
    guint i;

    for (i = 0; i < self->alias_rows->len; i++)
      adw_preferences_group_remove (self->alias_group, GTK_WIDGET (g_ptr_array_index (self->alias_rows, i)));

    g_ptr_array_set_size (self->alias_rows, 0);
  } else {
    self->alias_rows = g_ptr_array_new ();
  }

  service = stamp_account_get_mail_service (self->account);
  if (service)
    self->aliases = stamp_mail_service_get_aliases (service);

  if (!self->aliases)
    return;

  mails = g_hash_table_get_keys (self->aliases);

  for (GList *iter = mails; iter && iter->data; iter = g_list_next (iter)) {
    gchar *mail = iter->data;
    gchar *name = g_hash_table_lookup (self->aliases, mail);
    GtkWidget *row;
    GtkWidget *edit_button;

    row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), mail);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), name);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

    g_object_set_data_full (G_OBJECT (row), "email", g_strdup (mail), g_free);
    g_object_set_data_full (G_OBJECT (row), "name", g_strdup (name), g_free);
    g_signal_connect_object (row, "activated", G_CALLBACK (on_alias_activated), self, G_CONNECT_DEFAULT);

    edit_button = gtk_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (edit_button), "document-edit-symbolic");
    gtk_widget_add_css_class (edit_button, "flat");
    gtk_widget_set_valign (edit_button, GTK_ALIGN_CENTER);
    g_object_set_data (G_OBJECT (edit_button), "row", row);
    g_signal_connect (edit_button, "clicked", G_CALLBACK (on_alias_edit_clicked), self);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), edit_button);

    adw_preferences_group_add (self->alias_group, row);
    g_ptr_array_add (self->alias_rows, row);
  }

  g_list_free (mails);
}

static void
on_default_signature_selected (GObject    *combo,
                               GParamSpec *pspec,
                               gpointer    user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  guint selected = adw_combo_row_get_selected (self->default_signature);
  gint sig_idx = 0;

  if (selected == GTK_INVALID_LIST_POSITION || !self->account_settings)
    return;

  if (selected == 0) {
    g_settings_set_string (self->account_settings, STAMP_PREFS_MAIL_DEFAULT_SIGNATURE, "");
    return;
  }

  for (GList *iter = stamp_session_get_signatures (stamp_session_get_default ()); iter && iter->data; iter = g_list_next (iter), sig_idx++) {
    if (sig_idx == (gint)selected - 1) {
      StampSignature *sig = (StampSignature *)iter->data;

      g_settings_set_string (self->account_settings, STAMP_PREFS_MAIL_DEFAULT_SIGNATURE, e_source_get_uid (stamp_signature_get_source (sig)));
      return;
    }
  }
}

static void
refresh_default_signature (StampPreferencesAccount *self)
{
  GList *sigs;
  GtkStringList *list;
  g_autofree gchar *default_uid = NULL;
  gint selected = 0;
  gint sig_idx = 0;

  list = gtk_string_list_new (NULL);
  gtk_string_list_append (list, _("None"));

  sigs = stamp_session_get_signatures (stamp_session_get_default ());

  for (GList *iter = sigs; iter && iter->data; iter = g_list_next (iter)) {
    StampSignature *sig = iter->data;

    gtk_string_list_append (list, stamp_signature_get_name (sig));
  }

  default_uid = self->account_settings ? g_settings_get_string (self->account_settings, STAMP_PREFS_MAIL_DEFAULT_SIGNATURE) : g_strdup ("");
  for (GList *iter = sigs; iter && iter->data; iter = g_list_next (iter), sig_idx++) {
    StampSignature *sig = (StampSignature *)iter->data;

    if (g_strcmp0 (e_source_get_uid (stamp_signature_get_source (sig)), default_uid) == 0) {
      selected = sig_idx + 1;
      break;
    }
  }

  g_signal_handlers_block_by_func (self->default_signature, on_default_signature_selected, self);
  adw_combo_row_set_model (self->default_signature, G_LIST_MODEL (list));
  adw_combo_row_set_selected (self->default_signature, selected);
  g_signal_handlers_unblock_by_func (self->default_signature, on_default_signature_selected, self);
}

static void
on_notification_folder_toggled (GtkWidget *widget,
                                gpointer   user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  const gchar *full_name = g_object_get_data (G_OBJECT (widget), "folder-full-name");
  gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (widget));
  gchar **folders = g_settings_get_strv (self->account_settings, "notification-folders");
  gchar **new_folders;

  if (active)
    new_folders = g_strv_append ((const gchar * const *)folders, full_name);
  else
    new_folders = g_strv_remove ((const gchar * const *)folders, full_name);

  g_settings_set_strv (self->account_settings, "notification-folders", (const char * const *)new_folders);
  g_strfreev (new_folders);
}

static void
add_folder_recursive (StampPreferencesAccount *self,
                      CamelFolderInfo         *info)
{
  if (!info)
    return;

  if (!(info->flags & CAMEL_FOLDER_NOSELECT)) {
    g_auto (GStrv) folders = g_settings_get_strv (self->account_settings, "notification-folders");
    GtkWidget *row = adw_switch_row_new ();
    gboolean active = g_strv_contains ((const char **)folders, info->full_name);

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), info->display_name ? info->display_name : info->full_name);
    adw_switch_row_set_active (ADW_SWITCH_ROW (row), active);

    g_object_set_data_full (G_OBJECT (row), "folder-full-name", g_strdup (info->full_name), g_free);
    g_signal_connect (row, "notify::active", G_CALLBACK (on_notification_folder_toggled), self);

    adw_preferences_group_add (self->notification_folders_group, row);
    g_ptr_array_add (self->folder_rows, row);
  }

  add_folder_recursive (self, info->child);
}

static void
on_get_folder_info (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  CamelStore *store = CAMEL_STORE (source);
  g_autoptr (CamelFolderInfo) folder_info = NULL;
  g_autoptr (GError) error = NULL;

  folder_info = camel_store_get_folder_info_finish (store, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("%s: Error loading folders: %s", G_STRFUNC, error->message);
    return;
  }

  for (CamelFolderInfo *fi = folder_info; fi; fi = fi->next)
    add_folder_recursive (self, fi);
}

static void
load_folders (StampPreferencesAccount *self)
{
  StampMailService *mail_service = stamp_account_get_mail_service (self->account);
  CamelService *service;

  if (!mail_service)
    return;

  service = stamp_mail_service_get_service (mail_service);
  if (!service)
    return;

  camel_store_get_folder_info (CAMEL_STORE (service),
                               NULL,
                               CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_FAST,
                               G_PRIORITY_DEFAULT,
                               self->cancellable,
                               on_get_folder_info,
                               self);
}

static void
on_notification_mode_changed (GObject    *gobject,
                              GParamSpec *pspec,
                              gpointer    user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  guint selected = adw_combo_row_get_selected (self->notification_mode);
  const gchar *mode;

  switch (selected) {
    case 1:
      mode = "all";
      break;
    case 2:
      mode = "custom";
      break;
    default:
      mode = "inbox";
      break;
  }

  g_settings_set_string (self->account_settings, "notification-mode", mode);

  if (selected == 2) {
    gtk_widget_set_visible (GTK_WIDGET (self->notification_folders_group), TRUE);

    if (self->folder_rows->len == 0)
      load_folders (self);
  } else {
    gtk_widget_set_visible (GTK_WIDGET (self->notification_folders_group), FALSE);
  }
}

#define NUMBER_OF_COLORS 14

static const struct {
  const gchar *name;
  const gchar *title;
} ACCOUNT_COLORS[NUMBER_OF_COLORS] = {
  { "color1", N_("Blue") },
  { "color2", N_("Cyan") },
  { "color3", N_("Green") },
  { "color4", N_("Lime") },
  { "color5", N_("Yellow") },
  { "color6", N_("Gold") },
  { "color7", N_("Orange") },
  { "color8", N_("Raspberry") },
  { "color9", N_("Magenta") },
  { "color10", N_("Purple") },
  { "color11", N_("Violet") },
  { "color12", N_("Beige") },
  { "color13", N_("Brown") },
  { "color14", N_("Gray") },
};

static void
reload_badge_color_toggles (StampPreferencesAccount *self)
{
  g_autofree gchar *current_color = stamp_account_get_badge_color (self->account);
  gboolean is_auto = (!current_color || !*current_color || g_strcmp0 (current_color, "auto") == 0);
  GtkWidget *child;

  self->updating_badge_colors = TRUE;

  gtk_widget_set_sensitive (GTK_WIDGET (self->reset_badge_color_button), !is_auto);

  for (child = gtk_widget_get_first_child (GTK_WIDGET (self->badge_color_box));
       child;
       child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *button = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
    const gchar *color_name = g_object_get_data (G_OBJECT (button), "color-class");
    gboolean active = (!is_auto && g_strcmp0 (current_color, color_name) == 0);

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), active);
  }

  self->updating_badge_colors = FALSE;
}

static void
on_badge_color_toggled (StampPreferencesAccount *self,
                        GtkToggleButton         *toggle)
{
  GtkWidget *child;

  if (self->updating_badge_colors)
    return;

  if (gtk_toggle_button_get_active (toggle)) {
    const gchar *color_name = g_object_get_data (G_OBJECT (toggle), "color-class");

    self->updating_badge_colors = TRUE;
    for (child = gtk_widget_get_first_child (GTK_WIDGET (self->badge_color_box));
         child;
         child = gtk_widget_get_next_sibling (child)) {
      GtkWidget *button = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
      if (button != GTK_WIDGET (toggle))
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), FALSE);
    }
    self->updating_badge_colors = FALSE;

    gtk_widget_set_sensitive (GTK_WIDGET (self->reset_badge_color_button), TRUE);
    stamp_account_set_badge_color (self->account, color_name);
  } else {
    gboolean any_active = FALSE;
    for (child = gtk_widget_get_first_child (GTK_WIDGET (self->badge_color_box));
         child;
         child = gtk_widget_get_next_sibling (child)) {
      GtkWidget *button = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button))) {
        any_active = TRUE;
        break;
      }
    }

    if (!any_active) {
      gtk_widget_set_sensitive (GTK_WIDGET (self->reset_badge_color_button), FALSE);
      stamp_account_set_badge_color (self->account, "auto");
    }
  }
}

static void
on_reset_badge_color_clicked (StampPreferencesAccount *self)
{
  GtkWidget *child;

  self->updating_badge_colors = TRUE;
  for (child = gtk_widget_get_first_child (GTK_WIDGET (self->badge_color_box));
       child;
       child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *button = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), FALSE);
  }
  self->updating_badge_colors = FALSE;

  gtk_widget_set_sensitive (GTK_WIDGET (self->reset_badge_color_button), FALSE);
  stamp_account_set_badge_color (self->account, "auto");
}

static void
setup_badge_color (StampPreferencesAccount *self)
{
  guint i;

  gtk_flow_box_remove_all (self->badge_color_box);

  for (i = 0; i < NUMBER_OF_COLORS; i++) {
    GtkWidget *button = gtk_toggle_button_new ();
    GtkWidget *flow_child;

    gtk_widget_set_tooltip_text (button, _(ACCOUNT_COLORS[i].title));
    gtk_widget_add_css_class (button, "accent-button");
    gtk_widget_add_css_class (button, ACCOUNT_COLORS[i].name);

    g_object_set_data (G_OBJECT (button), "color-class", (gpointer)ACCOUNT_COLORS[i].name);

    g_signal_connect_object (button, "toggled",
                             G_CALLBACK (on_badge_color_toggled), self,
                             G_CONNECT_SWAPPED);

    gtk_flow_box_append (self->badge_color_box, button);

    flow_child = gtk_widget_get_parent (button);
    if (flow_child && GTK_IS_FLOW_BOX_CHILD (flow_child))
      gtk_widget_set_focusable (flow_child, FALSE);
  }

  g_signal_handlers_disconnect_by_func (self->reset_badge_color_button, on_reset_badge_color_clicked, self);
  g_signal_connect_object (self->reset_badge_color_button, "clicked",
                           G_CALLBACK (on_reset_badge_color_clicked), self,
                           G_CONNECT_SWAPPED);

  reload_badge_color_toggles (self);
}

static void
setup_notifications (StampPreferencesAccount *self)
{
  g_autofree char *settings_path = NULL;
  g_autoptr (GtkStringList) model = NULL;
  const gchar *mode;
  guint selected;

  settings_path = g_strconcat ("/org/tabos/stamp/mail/accounts/", stamp_account_get_uid (self->account), "/", NULL);
  self->account_settings = g_settings_new_with_path ("org.tabos.stamp.mail.accounts", settings_path);

  model = gtk_string_list_new ((const char *[]){_("Inbox"), _("All"), _("Custom"), NULL});
  adw_combo_row_set_model (self->notification_mode, G_LIST_MODEL (model));

  mode = g_settings_get_string (self->account_settings, "notification-mode");
  if (g_strcmp0 (mode, "all") == 0)
    selected = 1;
  else if (g_strcmp0 (mode, "custom") == 0)
    selected = 2;
  else
    selected = 0;

  adw_combo_row_set_selected (self->notification_mode, selected);
  g_free ((gchar *)mode);

  gtk_widget_set_visible (GTK_WIDGET (self->notification_folders_group), selected == 2);

  g_signal_connect_object (self->notification_mode, "notify::selected", G_CALLBACK (on_notification_mode_changed), self, G_CONNECT_DEFAULT);

  if (selected == 2)
    load_folders (self);
}

static void
on_cert_list_item_setup (GtkListItemFactory *factory,
                         GtkListItem        *item)
{
  GtkWidget *title = gtk_label_new (NULL);

  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_list_item_set_child (item, title);
}

static void
on_cert_list_item_bind (GtkListItemFactory *factory,
                        GtkListItem        *item)
{
  GtkWidget *title = gtk_list_item_get_child (item);
  GtkStringObject *string_obj = GTK_STRING_OBJECT (gtk_list_item_get_item (item));
  const gchar *text;

  if (!string_obj)
    return;

  text = gtk_string_object_get_string (string_obj);
  gtk_label_set_text (GTK_LABEL (title), text);
}

static GtkStringList *
enumerate_smime_certs (GPtrArray *nicknames)
{
  GtkStringList *list = gtk_string_list_new (NULL);
  CERTCertList *cert_list;
  CERTCertListNode *node;

  g_ptr_array_set_size (nicknames, 0);

  gtk_string_list_append (list, _("None"));
  g_ptr_array_add (nicknames, NULL);

  cert_list = PK11_ListCerts (PK11CertListAll, NULL);
  if (!cert_list)
    return list;

  for (node = CERT_LIST_HEAD (cert_list); !CERT_LIST_END (node, cert_list); node = CERT_LIST_NEXT (node)) {
    CERTCertificate *cert = node->cert;

    if (!cert->nickname || g_strcmp0 (cert->nickname, "") == 0)
      continue;

    /* if (!(cert->keyUsage & certificateUsageObjectSigner) && !(cert->keyUsage & certificateUsageEmailSigner)) */
    /*   continue; */

    gtk_string_list_append (list, cert->nickname);

    g_ptr_array_add (nicknames, g_strdup (cert->nickname));
  }

  CERT_DestroyCertList (cert_list);

  return list;
}

static gboolean
init_gpgme (void)
{
  static gsize initialized = 0;
  static gboolean available = FALSE;

  if (g_once_init_enter (&initialized)) {
    gpgme_error_t err;

    gpgme_check_version (NULL);
    gpgme_set_locale (NULL, LC_CTYPE, setlocale (LC_CTYPE, NULL));
#ifdef LC_MESSAGES
    gpgme_set_locale (NULL, LC_MESSAGES, setlocale (LC_MESSAGES, NULL));
#endif

    err = gpgme_engine_check_version (GPGME_PROTOCOL_OpenPGP);
    if (err != GPG_ERR_NO_ERROR)
      g_warning ("OpenPGP engine not available: %s", gpgme_strerror (err));
    else
      available = TRUE;

    g_once_init_leave (&initialized, 1);
  }

  return available;
}

static GtkStringList *
enumerate_pgp_keys (GPtrArray *key_ids)
{
  GtkStringList *list = gtk_string_list_new (NULL);
  gpgme_ctx_t ctx;
  gpgme_error_t err;
  gpgme_key_t key;

  if (key_ids)
    g_ptr_array_set_size (key_ids, 0);

  gtk_string_list_append (list, _("None"));
  if (key_ids)
    g_ptr_array_add (key_ids, NULL);

  if (!init_gpgme ())
    return list;

  err = gpgme_new (&ctx);
  if (err != GPG_ERR_NO_ERROR) {
    g_warning ("Failed to create gpgme context: %s", gpgme_strerror (err));
    return list;
  }

  gpgme_set_protocol (ctx, GPGME_PROTOCOL_OpenPGP);
  gpgme_set_keylist_mode (ctx, GPGME_KEYLIST_MODE_LOCAL);

  err = gpgme_op_keylist_start (ctx, NULL, 0);
  if (err != GPG_ERR_NO_ERROR) {
    g_warning ("Failed to list PGP keys: %s", gpgme_strerror (err));
    gpgme_release (ctx);
    return list;
  }

  while (gpgme_op_keylist_next (ctx, &key) == GPG_ERR_NO_ERROR) {
    const gchar *key_id = key->subkeys ? key->subkeys->keyid : NULL;
    const gchar *uid = key->uids ? key->uids->uid : NULL;

    if (key_id && (key->secret || key->has_sign || key->has_encrypt)) {
      if (uid)
        gtk_string_list_append (list, uid);
      else
        gtk_string_list_append (list, key_id);

      if (key_ids)
        g_ptr_array_add (key_ids, g_strdup (key_id));
    }

    gpgme_key_unref (key);
  }

  gpgme_op_keylist_end (ctx);
  gpgme_release (ctx);

  return list;
}

static void
on_pgp_key_selected (GObject    *combo,
                     GParamSpec *pspec,
                     gpointer    user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  guint selected = adw_combo_row_get_selected (self->pgp_key);

  if (selected > 0 && self->pgp_key_ids && selected < self->pgp_key_ids->len)
    g_settings_set_string (self->account_settings, "pgp-key-id", g_ptr_array_index (self->pgp_key_ids, selected));
  else
    g_settings_set_string (self->account_settings, "pgp-key-id", "");
}

static void
on_smime_sign_cert_selected (GObject    *combo,
                             GParamSpec *pspec,
                             gpointer    user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  guint selected = adw_combo_row_get_selected (self->smime_sign_cert);

  if (selected > 0 && self->smime_cert_nicknames && selected < self->smime_cert_nicknames->len)
    g_settings_set_string (self->account_settings, "smime-sign-cert", g_ptr_array_index (self->smime_cert_nicknames, selected));
  else
    g_settings_set_string (self->account_settings, "smime-sign-cert", "");
}

static void
on_smime_encrypt_cert_selected (GObject    *combo,
                                GParamSpec *pspec,
                                gpointer    user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  guint selected = adw_combo_row_get_selected (self->smime_encrypt_cert);

  if (selected > 0 && self->smime_cert_nicknames && selected < self->smime_cert_nicknames->len)
    g_settings_set_string (self->account_settings, "smime-encrypt-cert", g_ptr_array_index (self->smime_cert_nicknames, selected));
  else
    g_settings_set_string (self->account_settings, "smime-encrypt-cert", "");
}

static void
setup_crypto (StampPreferencesAccount *self)
{
  GtkListItemFactory *factory;
  GtkStringList *pgp_keys;
  GtkStringList *smime_certs;
  g_autofree char *key_id = NULL;
  g_autofree char *sign_val = NULL;
  g_autofree char *enc_val = NULL;

  g_signal_handlers_block_by_func (self->smime_encrypt_cert, on_smime_encrypt_cert_selected, self);
  g_signal_handlers_block_by_func (self->smime_sign_cert, on_smime_sign_cert_selected, self);
  g_signal_handlers_block_by_func (self->pgp_key, on_pgp_key_selected, self);

  g_clear_pointer (&self->pgp_key_ids, g_ptr_array_unref);
  self->pgp_key_ids = g_ptr_array_new_full (0, g_free);
  pgp_keys = enumerate_pgp_keys (self->pgp_key_ids);
  adw_combo_row_set_model (self->pgp_key, G_LIST_MODEL (pgp_keys));

  g_clear_pointer (&self->smime_cert_nicknames, g_ptr_array_unref);
  self->smime_cert_nicknames = g_ptr_array_new_full (0, g_free);
  smime_certs = enumerate_smime_certs (self->smime_cert_nicknames);
  adw_combo_row_set_model (self->smime_sign_cert, G_LIST_MODEL (smime_certs));
  adw_combo_row_set_model (self->smime_encrypt_cert, G_LIST_MODEL (smime_certs));

  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (on_cert_list_item_setup), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (on_cert_list_item_bind), NULL);

  adw_combo_row_set_list_factory (self->pgp_key, factory);
  adw_combo_row_set_list_factory (self->smime_sign_cert, factory);
  adw_combo_row_set_list_factory (self->smime_encrypt_cert, factory);

  key_id = g_settings_get_string (self->account_settings, "pgp-key-id");
  if (key_id && strlen (key_id) > 0) {
    for (guint i = 1; self->pgp_key_ids && i < self->pgp_key_ids->len; i++) {
      const gchar *candidate = g_ptr_array_index (self->pgp_key_ids, i);

      if (g_strcmp0 (candidate, key_id) == 0) {
        adw_combo_row_set_selected (self->pgp_key, i);
        break;
      }
    }
  }

  sign_val = g_settings_get_string (self->account_settings, "smime-sign-cert");
  if (sign_val && strlen (sign_val) > 0) {
    for (guint i = 1; self->smime_cert_nicknames && i < self->smime_cert_nicknames->len; i++) {
      const gchar *candidate = g_ptr_array_index (self->smime_cert_nicknames, i);

      if (g_strcmp0 (candidate, sign_val) == 0) {
        adw_combo_row_set_selected (self->smime_sign_cert, i);
        break;
      }
    }
  }

  enc_val = g_settings_get_string (self->account_settings, "smime-encrypt-cert");
  if (enc_val && strlen (enc_val) > 0) {
    for (guint i = 1; self->smime_cert_nicknames && i < self->smime_cert_nicknames->len; i++) {
      const gchar *candidate = g_ptr_array_index (self->smime_cert_nicknames, i);

      if (g_strcmp0 (candidate, enc_val) == 0) {
        adw_combo_row_set_selected (self->smime_encrypt_cert, i);
        break;
      }
    }
  }

  g_signal_handlers_unblock_by_func (self->pgp_key, on_pgp_key_selected, self);
  g_signal_handlers_unblock_by_func (self->smime_sign_cert, on_smime_sign_cert_selected, self);
  g_signal_handlers_unblock_by_func (self->smime_encrypt_cert, on_smime_encrypt_cert_selected, self);
}

static void
on_out_of_office_activated (AdwActionRow *row,
                            gpointer      user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));
  GtkWidget *page = stamp_preferences_out_of_office_new (self->account);

  adw_navigation_view_push (ADW_NAVIGATION_VIEW (parent), ADW_NAVIGATION_PAGE (page));
}

static void
set_account (StampPreferencesAccount *self,
             StampAccount            *account)
{
  g_autofree char *path = NULL;
  const gchar *name;

  g_set_object (&self->account, account);
  name = stamp_account_get_name (self->account);
  adw_window_title_set_title (self->window_title, name);

  path = g_strdup_printf ("/org/tabos/stamp/mail/accounts/%s/", stamp_account_get_uid (self->account));
  self->account_settings = g_settings_new_with_path ("org.tabos.stamp.mail.accounts", path);

  refresh_default_signature (self);
  refresh_alias_list (self);

  setup_badge_color (self);
  setup_notifications (self);
  setup_crypto (self);

  gtk_widget_set_visible (GTK_WIDGET (self->out_of_office_group), stamp_out_of_office_supported (self->account));
}

static void
on_add_alias_clicked (GtkWidget *button,
                      gpointer   user_data)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (user_data);
  AdwNavigationPage *page;
  GtkWidget *parent;

  parent = gtk_widget_get_parent (GTK_WIDGET (self));
  page = ADW_NAVIGATION_PAGE (stamp_preferences_account_editor_new (self->account, NULL, NULL));
  adw_navigation_view_push (ADW_NAVIGATION_VIEW (parent), page);
}

static void
stamp_preferences_account_get_property (GObject    *object,
                                        guint       property_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (object);

  switch ((StampPreferencesAccountProps)property_id) {
    case PROP_ACCOUNT:
      g_value_set_object (value, self->account);
      break;
  }
}

static void
stamp_preferences_account_set_property (GObject      *object,
                                        guint         property_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (object);

  switch ((StampPreferencesAccountProps)property_id) {
    case PROP_ACCOUNT:
      set_account (self, g_value_get_object (value));
      break;
  }
}

static void
stamp_preferences_account_dispose (GObject *object)
{
  StampPreferencesAccount *self = STAMP_PREFERENCES_ACCOUNT (object);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->account);
  g_clear_object (&self->account_settings);

  if (self->folder_rows)
    g_ptr_array_unref (self->folder_rows);

  if (self->pgp_key_ids)
    g_ptr_array_unref (self->pgp_key_ids);

  if (self->smime_cert_nicknames)
    g_ptr_array_unref (self->smime_cert_nicknames);

  G_OBJECT_CLASS (stamp_preferences_account_parent_class)->dispose (object);
}

void
stamp_preferences_account_class_init (StampPreferencesAccountClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = stamp_preferences_account_dispose;
  gobject_class->get_property = stamp_preferences_account_get_property;
  gobject_class->set_property = stamp_preferences_account_set_property;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/stamp-preferences-account.ui");

  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, window_title);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, page);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, default_signature);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, badge_color_box);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, reset_badge_color_button);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, alias_group);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, notification_mode);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, notification_folders_group);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, pgp_key);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, smime_sign_cert);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, smime_encrypt_cert);

  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccount, out_of_office_group);
  gtk_widget_class_bind_template_callback (widget_class, on_out_of_office_activated);
  gtk_widget_class_bind_template_callback (widget_class, on_add_alias_clicked);

  props[PROP_ACCOUNT] = g_param_spec_object ("account",
                                             NULL,
                                             NULL,
                                             STAMP_TYPE_ACCOUNT,
                                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (props), props);
}

void
stamp_preferences_account_init (StampPreferencesAccount *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->cancellable = g_cancellable_new ();
  self->folder_rows = g_ptr_array_new ();

  g_signal_connect (self->default_signature, "notify::selected", G_CALLBACK (on_default_signature_selected), self);
  g_signal_connect_swapped (self, "shown", G_CALLBACK (refresh_default_signature), self);
  g_signal_connect_swapped (self, "shown", G_CALLBACK (refresh_alias_list), self);
  g_signal_connect_swapped (self, "shown", G_CALLBACK (setup_crypto), self);

  g_signal_connect (self->pgp_key, "notify::selected", G_CALLBACK (on_pgp_key_selected), self);
  g_signal_connect (self->smime_sign_cert, "notify::selected", G_CALLBACK (on_smime_sign_cert_selected), self);
  g_signal_connect (self->smime_encrypt_cert, "notify::selected", G_CALLBACK (on_smime_encrypt_cert_selected), self);
}

GtkWidget *
stamp_preferences_account_new (StampAccount *account)
{
  return g_object_new (STAMP_TYPE_PREFERENCES_ACCOUNT, "account", account, NULL);
}

struct _StampPreferencesAccountEditor {
  AdwNavigationPage parent_instance;

  AdwWindowTitle *editor_window_title;
  AdwEntryRow *mail_row;
  AdwEntryRow *name_row;
  AdwButtonRow *save;
  AdwButtonRow *remove;

  StampAccount *account;
  gchar *email;
  gchar *name;
  GHashTable *aliases;
};

G_DEFINE_FINAL_TYPE (StampPreferencesAccountEditor, stamp_preferences_account_editor, ADW_TYPE_NAVIGATION_PAGE);

static void
on_save_clicked (GtkWidget *button,
                 gpointer   user_data)
{
  StampPreferencesAccountEditor *self = STAMP_PREFERENCES_ACCOUNT_EDITOR (user_data);
  const gchar *mail = gtk_editable_get_text (GTK_EDITABLE (self->mail_row));
  const gchar *name = gtk_editable_get_text (GTK_EDITABLE (self->name_row));
  GtkWidget *parent;

  if (g_strcmp0 (mail, "") != 0) {
    StampMailService *service;

    if (self->email && g_strcmp0 (self->email, mail) != 0) {
      g_hash_table_remove (self->aliases, self->email);
    }

    g_hash_table_insert (self->aliases, g_strdup (mail), g_strdup (name));

    service = stamp_account_get_mail_service (self->account);
    stamp_mail_service_set_aliases (service, self->aliases);
  }

  parent = gtk_widget_get_parent (GTK_WIDGET (self));
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (parent));
}

static void
on_remove_clicked (GtkWidget *button,
                   gpointer   user_data)
{
  StampPreferencesAccountEditor *self = STAMP_PREFERENCES_ACCOUNT_EDITOR (user_data);
  GtkWidget *parent;

  if (self->email) {
    StampMailService *service;

    g_hash_table_remove (self->aliases, self->email);

    service = stamp_account_get_mail_service (self->account);
    stamp_mail_service_set_aliases (service, self->aliases);
  }

  parent = gtk_widget_get_parent (GTK_WIDGET (self));
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (parent));
}

static void
on_entry_changed (GtkWidget *button,
                  gpointer   user_data)
{
  StampPreferencesAccountEditor *self = STAMP_PREFERENCES_ACCOUNT_EDITOR (user_data);
  const gchar *mail = gtk_editable_get_text (GTK_EDITABLE (self->mail_row));

  gtk_widget_set_sensitive (GTK_WIDGET (self->save), strlen (mail) != 0);
}

static void
stamp_preferences_account_editor_get_property (GObject    *object,
                                               guint       property_id,
                                               GValue     *value,
                                               GParamSpec *pspec)
{
}

static void
stamp_preferences_account_editor_set_property (GObject      *object,
                                               guint         property_id,
                                               const GValue *value,
                                               GParamSpec   *pspec)
{
}

static void
stamp_preferences_account_editor_dispose (GObject *object)
{
  StampPreferencesAccountEditor *self = STAMP_PREFERENCES_ACCOUNT_EDITOR (object);

  g_clear_object (&self->account);
  g_clear_pointer (&self->email, g_free);
  g_clear_pointer (&self->name, g_free);

  G_OBJECT_CLASS (stamp_preferences_account_editor_parent_class)->dispose (object);
}

void
stamp_preferences_account_editor_class_init (StampPreferencesAccountEditorClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = stamp_preferences_account_editor_dispose;
  gobject_class->get_property = stamp_preferences_account_editor_get_property;
  gobject_class->set_property = stamp_preferences_account_editor_set_property;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/stamp-preferences-account-editor.ui");

  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccountEditor, editor_window_title);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccountEditor, mail_row);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccountEditor, name_row);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccountEditor, save);
  gtk_widget_class_bind_template_child (widget_class, StampPreferencesAccountEditor, remove);

  gtk_widget_class_bind_template_callback (widget_class, on_save_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_remove_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_entry_changed);
}

void
stamp_preferences_account_editor_init (StampPreferencesAccountEditor *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

GtkWidget *
stamp_preferences_account_editor_new (StampAccount *account,
                                      const gchar  *email,
                                      const gchar  *name)
{
  StampPreferencesAccountEditor *self;
  StampMailService *service;

  self = g_object_new (STAMP_TYPE_PREFERENCES_ACCOUNT_EDITOR, NULL);

  self->account = g_object_ref (account);
  self->email = g_strdup (email);
  self->name = g_strdup (name);

  if (email) {
    adw_window_title_set_title (self->editor_window_title, _("Edit Alias"));
  } else {
    adw_window_title_set_title (self->editor_window_title, _("New Alias"));
  }

  service = stamp_account_get_mail_service (account);
  self->aliases = stamp_mail_service_get_aliases (service);
  if (!self->aliases)
    self->aliases = g_hash_table_new_full (camel_strcase_hash, camel_strcase_equal, g_free, g_free);

  gtk_editable_set_text (GTK_EDITABLE (self->mail_row), email ? email : "");
  gtk_editable_set_text (GTK_EDITABLE (self->name_row), name ? name : "");

  gtk_widget_set_visible (GTK_WIDGET (self->remove), email != NULL);

  return GTK_WIDGET (self);
}
