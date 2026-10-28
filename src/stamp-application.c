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

#include "config.h"

#include "stamp-application.h"

#include <glib/gi18n.h>
#include <pk11pub.h>

#include "stamp-composer.h"
#include "stamp-helper.h"
#include "stamp-preferences.h"
#include "stamp-scheduler.h"
#include "stamp-session.h"
#include "stamp-window.h"

struct _StampApplication {
  AdwApplication parent_instance;

  gboolean start_hidden;
  gboolean startup_finished;
  GQueue *pending_mailto;
  StampSession *session;
  gboolean quit_requested;
  gboolean quit_pending_confirmed;
  StampComposer *quit_composer;
  guint quit_source_id;
};

typedef struct {
  GMainLoop *loop;
  GtkWidget *entry;
  gchar *password;
} PasswordPromptData;

G_DEFINE_FINAL_TYPE (StampApplication, stamp_application, ADW_TYPE_APPLICATION);

StampApplication *
stamp_application_new (const gchar       *application_id,
                       GApplicationFlags  flags)
{
  StampApplication *stamp_application;

  g_return_val_if_fail (application_id != NULL, NULL);

  stamp_application = g_object_new (STAMP_TYPE_APPLICATION,
                                    "application-id", application_id,
                                    "flags", flags,
                                    "resource-base-path", "/org/tabos/stamp",
                                    NULL);

  return stamp_application;
}

static void
stamp_application_activate (GApplication *app)
{
  StampApplication *self = STAMP_APPLICATION (app);
  GtkWindow *main_window = NULL;
  GList *windows = NULL;

  windows = gtk_application_get_windows (GTK_APPLICATION (app));
  for (GList *iter = windows; iter; iter = iter->next) {
    GtkWidget *win = iter->data;

    if (STAMP_IS_WINDOW (win)) {
      main_window = GTK_WINDOW (win);
      break;
    }
  }

  if (!main_window)
    main_window = g_object_new (STAMP_TYPE_WINDOW, "application", app, NULL);

  if (self->start_hidden) {
    gtk_widget_set_visible (GTK_WIDGET (main_window), FALSE);
    self->start_hidden = FALSE;
    return;
  }

  gtk_window_present (main_window);
}

static void
stamp_application_open_composer (StampApplication *self,
                                 const gchar      *mailto_uri)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *to = NULL;
  g_autoptr (GUri) mailto = NULL;
  GtkWidget *composer;
  const gchar *uri_query;

  mailto = g_uri_parse (mailto_uri, G_URI_FLAGS_NONE, &error);
  if (error) {
    g_warning ("%s: Argument is not a URL: %s", G_STRFUNC, error->message);
    return;
  }

  if (g_strcmp0 (g_uri_get_scheme (mailto), "mailto") != 0) {
    g_warning ("%s: Cannot open non-mailto URL", G_STRFUNC);
    return;
  }

  to = g_uri_unescape_string (g_uri_get_path (mailto), NULL);

  composer = stamp_composer_new (NULL);
  stamp_composer_set_to (STAMP_COMPOSER (composer), to);
  gtk_window_present (GTK_WINDOW (composer));

  uri_query = g_uri_get_query (mailto);
  if (uri_query) {
    GUriParamsIter qp;
    gchar *name;
    gchar *value;
    g_autoptr (GString) body = g_string_new (NULL);
    g_autofree char *escaped = NULL;
    g_autofree char *markup = NULL;
    g_autoptr (GRegex) regex = NULL;
    guint has_attachments = 0;

    g_uri_params_iter_init (&qp, uri_query, -1, "&", G_URI_PARAMS_NONE);
    while (g_uri_params_iter_next (&qp, &name, &value, &error)) {
      if (!name || !value || error) {
        break;
      }

      if (g_strcmp0 (name, "subject") == 0)
        stamp_composer_set_subject (STAMP_COMPOSER (composer), value);
      else if (g_strcmp0 (name, "body") == 0)
        g_string_append (body, value);
      else if (g_strcmp0 (name, "attachment") == 0) {
        stamp_composer_add_attachment (STAMP_COMPOSER (composer), value);
        has_attachments++;
      } else
        g_string_append_printf (body, "&%s=%s", name, value);

      g_free (name);
      g_free (value);
    }

    escaped = g_markup_escape_text (body->str, body->len);
    regex = g_regex_new ("\n", 0, 0, NULL);
    markup = g_regex_replace_literal (regex, escaped, -1, 0, "<br/>", 0, NULL);

    stamp_composer_set_body (STAMP_COMPOSER (composer), markup);

    if (has_attachments) {
      const gchar *dialog_title = g_dngettext (GETTEXT_PACKAGE, "Review attachment before sending.", "Review attachments before sending.", has_attachments);
      g_autofree gchar *dialog_body = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                                                    "There had been added %d attachment. Make sure it does not contain any sensitive information before sending the message.",
                                                                    "There had been added %d attachments. Make sure they do not contain any sensitive information before sending the message.",
                                                                    has_attachments),
                                                       has_attachments);


      AdwDialog *dialog = adw_alert_dialog_new (dialog_title, dialog_body);

      adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "understood", _("_Understood"));

      adw_dialog_present (dialog, GTK_WIDGET (composer));
    }
  }
}

static void
on_startup_finished (StampSession *session,
                     gpointer      user_data)
{
  StampApplication *self = STAMP_APPLICATION (user_data);
  gchar *mailto_uri;

  self->startup_finished = TRUE;

  while ((mailto_uri = g_queue_pop_head (self->pending_mailto))) {
    stamp_application_open_composer (self, mailto_uri);
    g_free (mailto_uri);
  }
}

static gint
stamp_application_command_line (GApplication            *app,
                                GApplicationCommandLine *cmdline)
{
  g_auto (GStrv) argv = NULL;
  gint argc;

  argv = g_application_command_line_get_arguments (cmdline, &argc);
  stamp_application_activate (app);

  for (gint i = 1; argv[i]; i++) {
    StampApplication *self = STAMP_APPLICATION (app);
    g_autoptr (GUri) mailto = NULL;

    mailto = g_uri_parse (argv[i], G_URI_FLAGS_NONE, NULL);
    if (g_uri_get_scheme (mailto) == NULL ||
        g_strcmp0 (g_uri_get_scheme (mailto), "mailto") != 0) {
      g_warning ("%s: Cannot open non-mailto URL: %s", G_STRFUNC, argv[i]);
      continue;
    }

    if (self->startup_finished) {
      stamp_application_open_composer (self, argv[i]);
    } else {
      g_queue_push_tail (self->pending_mailto, g_strdup (argv[i]));
    }
  }

  return 0;
}

static void
on_password_dialog_response (AdwAlertDialog *dialog,
                             const gchar    *response,
                             gpointer        user_data)
{
  PasswordPromptData *data = user_data;

  if (g_strcmp0 (response, "decrypt") == 0)
    data->password = g_strdup (gtk_editable_get_text (GTK_EDITABLE (data->entry)));

  g_main_loop_quit (data->loop);
}

static void
on_pk11_password (StampSession  *session,
                  PK11SlotInfo  *slot,
                  PRBool         retry,
                  gchar        **password,
                  gpointer       user_data)
{
  StampApplication *self = STAMP_APPLICATION (user_data);
  g_autofree char *prompt = NULL;
  g_autofree char *slot_name = g_strdup (PK11_GetSlotName (slot));
  g_autofree char *token_name = g_strdup (PK11_GetTokenName (slot));
  g_autoptr (AdwAlertDialog) dialog = NULL;
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  PasswordPromptData data = { .loop = loop };
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (self));
  gulong response_handler;

  *password = NULL;

  g_strchomp (slot_name);

  if (token_name)
    g_strchomp (token_name);

  if (token_name && *token_name && g_ascii_strcasecmp (slot_name, token_name) != 0)
    prompt = g_strdup_printf (_("Enter the password for “%s”, token “%s”"), slot_name, token_name);
  else
    prompt = g_strdup_printf (_("Enter the password for “%s”"), slot_name);

  dialog = ADW_ALERT_DIALOG (g_object_ref_sink (adw_alert_dialog_new (_("Enter Password"), prompt)));
  adw_alert_dialog_add_response (dialog, "cancel", _("Cancel"));
  adw_alert_dialog_add_response (dialog, "decrypt", _("Decrypt"));
  adw_alert_dialog_set_default_response (dialog, "decrypt");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  response_handler = g_signal_connect (dialog, "response", G_CALLBACK (on_password_dialog_response), &data);

  data.entry = gtk_entry_new ();
  gtk_entry_set_activates_default (GTK_ENTRY (data.entry), TRUE);
  gtk_entry_set_visibility (GTK_ENTRY (data.entry), FALSE);
  adw_alert_dialog_set_extra_child (dialog, data.entry);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (window));
  gtk_widget_grab_focus (data.entry);

  g_main_loop_run (loop);

  g_signal_handler_disconnect (dialog, response_handler);
  *password = g_steal_pointer (&data.password);
}

static void
stamp_application_startup (GApplication *app)
{
  StampApplication *self = STAMP_APPLICATION (app);

  G_APPLICATION_CLASS (stamp_application_parent_class)->startup (app);

  self->session = stamp_session_get_default ();
  g_signal_connect_object (self->session, "pk11-password", G_CALLBACK (on_pk11_password), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (self->session, "startup-finished", G_CALLBACK (on_startup_finished), self, G_CONNECT_DEFAULT);
}

static gint
stamp_application_handle_local_options (GApplication *application,
                                        GVariantDict *options)
{
  StampApplication *self = STAMP_APPLICATION (application);

  self->start_hidden = g_variant_dict_contains (options, "hidden") && !gtk_application_get_active_window (GTK_APPLICATION (self));

  return -1;
}

static void
stamp_application_dispose (GObject *object)
{
  StampApplication *self = STAMP_APPLICATION (object);

  g_clear_handle_id (&self->quit_source_id, g_source_remove);
  g_clear_object (&self->quit_composer);
  g_clear_object (&self->session);

  if (self->pending_mailto) {
    g_queue_free_full (self->pending_mailto, g_free);
    self->pending_mailto = NULL;
  }

  G_OBJECT_CLASS (stamp_application_parent_class)->dispose (object);
}

static void
stamp_application_class_init (StampApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  app_class->activate = stamp_application_activate;
  app_class->command_line = stamp_application_command_line;
  app_class->startup = stamp_application_startup;
  app_class->handle_local_options = stamp_application_handle_local_options;

  object_class->dispose = stamp_application_dispose;
}

static void
stamp_application_about_action (GSimpleAction *action,
                                GVariant      *parameter,
                                gpointer       user_data)
{
  static const gchar *developers[] = {"Jan-Michael Brummer", "Michael Catanzaro", NULL};
  static const gchar *designers[] = {"Tobias Bernard", NULL};
  StampApplication *self = STAMP_APPLICATION (user_data);
  GtkWindow *window = NULL;
  AdwDialog *dialog;

  g_assert (STAMP_IS_APPLICATION (self));

  window = gtk_application_get_active_window (GTK_APPLICATION (self));

  dialog = adw_about_dialog_new_from_appdata ("/org/tabos/stamp/org.tabos.stamp.metainfo.xml", PACKAGE_VERSION);
  adw_about_dialog_set_translator_credits (ADW_ABOUT_DIALOG (dialog), _("translator-credits"));
  adw_about_dialog_set_developers (ADW_ABOUT_DIALOG (dialog), developers);
  adw_about_dialog_set_designers (ADW_ABOUT_DIALOG (dialog), designers);
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog), "© 2024-2026 Jan-Michael Brummer");
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dialog), VERSION);
  adw_dialog_present (dialog, GTK_WIDGET (window));
}

static void cancel_quit (StampApplication *self);
static void continue_quit (gpointer user_data);

static void
on_composer_close_cancelled (StampComposer *composer,
                             gpointer       user_data)
{
  cancel_quit (STAMP_APPLICATION (user_data));
}

static void
clear_quit_composer (StampApplication *self)
{
  if (self->quit_composer)
    g_signal_handlers_disconnect_by_func (self->quit_composer, on_composer_close_cancelled, self);
  g_clear_object (&self->quit_composer);
}

static void
cancel_quit (StampApplication *self)
{
  g_clear_handle_id (&self->quit_source_id, g_source_remove);
  clear_quit_composer (self);
  self->quit_pending_confirmed = FALSE;

  if (self->quit_requested) {
    self->quit_requested = FALSE;
    g_application_release (G_APPLICATION (self));
  }
}

static void
queue_continue_quit (StampApplication *self)
{
  if (!self->quit_source_id)
    self->quit_source_id = g_idle_add_once (continue_quit, self);
}

static void
on_quit_window_removed (GtkApplication *application,
                        GtkWindow      *window,
                        gpointer        user_data)
{
  StampApplication *self = STAMP_APPLICATION (application);

  if (self->quit_requested && GTK_WINDOW (self->quit_composer) == window) {
    clear_quit_composer (self);
    queue_continue_quit (self);
  }
}

static void
stamp_application_quit_action (GSimpleAction *action,
                               GVariant      *parameter,
                               gpointer       user_data);

static void
on_quit_response (AdwAlertDialog *dialog,
                  const gchar    *response,
                  gpointer        user_data)
{
  StampApplication *self = STAMP_APPLICATION (user_data);

  if (g_strcmp0 (response, "quit") == 0) {
    self->quit_pending_confirmed = TRUE;
    if (self->quit_requested)
      queue_continue_quit (self);
    else
      stamp_application_quit_action (NULL, NULL, self);
  } else {
    cancel_quit (self);
  }
}

gboolean
stamp_application_confirm_quit (void)
{
  StampApplication *self = STAMP_APPLICATION (g_application_get_default ());
  GtkWindow *parent;
  AdwDialog *dialog;
  g_autofree char *body = NULL;
  guint pending;

  if (self->quit_pending_confirmed)
    return TRUE;

  pending = stamp_scheduler_get_pending_count ();
  if (pending == 0)
    return TRUE;

  body = g_strdup_printf (ngettext ("%u message is scheduled to be sent later and will only be sent while the application is running. Quitting now will prevent it from being sent.",
                                    "%u messages are scheduled to be sent later and will only be sent while the application is running. Quitting now will prevent them from being sent.",
                                    pending),
                          pending);

  dialog = adw_alert_dialog_new (_("Scheduled messages pending"), body);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("_Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "quit", _("_Quit"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "quit", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

  parent = gtk_application_get_active_window (GTK_APPLICATION (self));
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_quit_response), self, G_CONNECT_DEFAULT);
  adw_dialog_present (dialog, GTK_WIDGET (parent));

  return FALSE;
}

static void
continue_quit (gpointer user_data)
{
  StampApplication *self = STAMP_APPLICATION (user_data);
  GList *windows = NULL;

  self->quit_source_id = 0;
  if (!self->quit_requested || self->quit_composer)
    return;

  /* Wait for each composer to finish saving or to cancel its close request. */
  windows = gtk_application_get_windows (GTK_APPLICATION (self));
  for (GList *iter = windows; iter; iter = iter->next) {
    if (STAMP_IS_COMPOSER (iter->data)) {
      self->quit_composer = g_object_ref (iter->data);
      g_signal_connect_object (self->quit_composer, "close-cancelled",
                               G_CALLBACK (on_composer_close_cancelled), self, G_CONNECT_DEFAULT);
      gtk_window_present (GTK_WINDOW (self->quit_composer));
      gtk_window_close (GTK_WINDOW (self->quit_composer));
      return;
    }
  }

  if (!stamp_application_confirm_quit ())
    return;

  windows = gtk_application_get_windows (GTK_APPLICATION (self));

  while (windows && windows->data) {
    GtkWindow *window = GTK_WINDOW (windows->data);

    windows = windows->next;
    gtk_window_destroy (GTK_WINDOW (window));
  }

  cancel_quit (self);
  g_application_quit (G_APPLICATION (self));
}

static void
stamp_application_quit_action (GSimpleAction *action,
                               GVariant      *parameter,
                               gpointer       user_data)
{
  StampApplication *self = STAMP_APPLICATION (user_data);

  if (self->quit_requested)
    return;

  self->quit_requested = TRUE;
  g_application_hold (G_APPLICATION (self));
  continue_quit (self);
}

static void
stamp_application_accounts_action (GSimpleAction *action,
                                   GVariant      *parameter,
                                   gpointer       user_data)
{
  stamp_launch_goa ();
}

static void
stamp_application_preferences_action (GSimpleAction *action,
                                      GVariant      *parameter,
                                      gpointer       user_data)
{
  StampApplication *self = STAMP_APPLICATION (user_data);
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (self));
  GtkWidget *preferences = stamp_preferences_new ();

  adw_dialog_present (ADW_DIALOG (preferences), GTK_WIDGET (window));
}

static void
stamp_application_show_message (GSimpleAction *action,
                                GVariant      *parameter,
                                gpointer       user_data)
{
  StampWindow *window = stamp_get_main_window ();

  stamp_window_show_mail_view (window);
  /* TODO: Select folder and message? */
  gtk_window_present (GTK_WINDOW (window));
}

static const GActionEntry app_actions[] = {
  { "accounts", stamp_application_accounts_action },
  { "quit", stamp_application_quit_action },
  { "about", stamp_application_about_action },
  { "preferences", stamp_application_preferences_action },
  { "show-message", stamp_application_show_message, "s" },
};

static void
stamp_application_init (StampApplication *self)
{
  self->pending_mailto = g_queue_new ();
  g_signal_connect (self, "window-removed", G_CALLBACK (on_quit_window_removed), NULL);

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   app_actions,
                                   G_N_ELEMENTS (app_actions),
                                   self);

  g_application_add_main_option (G_APPLICATION (self), "hidden", 'h', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, _("Start window hidden"), NULL);

  gtk_application_set_accels_for_action (GTK_APPLICATION (self), "app.quit", (const char *[]) { "<primary>q", NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (self), "app.preferences", (const char *[]) { "<primary>comma", NULL });
}
