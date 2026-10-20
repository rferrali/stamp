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

#include "stamp-webview.h"

struct _StampWebView {
  WebKitWebView parent_instance;

  GHashTable *internal_resources;
  gint32 width_request;
  gint32 height_request;
  gboolean loaded;
  gchar *queued_body_content;
  gboolean queued_load_images;
  GCancellable *cancellable;
  gboolean body_html_changed;

  cid_handler_func cid_handler;
  gpointer cid_handler_user_data;
  guint page_size_timeout_handler;
};

G_DEFINE_FINAL_TYPE (StampWebView, stamp_webview, WEBKIT_TYPE_WEB_VIEW);

typedef enum {
  PROP_SIZE_REQUEST = 1,
} StampWebViewProps;

static GParamSpec *properties[PROP_SIZE_REQUEST + 1];

enum {
  IMAGE_LOAD_BLOCKED,
  SELECTION_CHANGED,
  LOADED,
  LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = { 0 };
static WebKitWebView *shared_process_anchor;

static void
stamp_webview_get_property (GObject    *object,
                            guint       property_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  StampWebView *self = STAMP_WEB_VIEW (object);

  switch ((StampWebViewProps)property_id) {
    case PROP_SIZE_REQUEST:
      g_value_set_boolean (value, self->loaded);
      break;
  }
}

static void
stamp_web_view_set_property (GObject      *object,
                             guint         property_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  switch ((StampWebViewProps)property_id) {
    case PROP_SIZE_REQUEST:
      break;
  }
}

static void
on_cid_request (WebKitURISchemeRequest *request,
                gpointer                user_data)
{
  StampWebView *self = STAMP_WEB_VIEW (webkit_uri_scheme_request_get_web_view (request));
  GInputStream *stream;

  stream = self->cid_handler ? self->cid_handler (request, self->cid_handler_user_data) : NULL;
  if (!stream) {
    GError *error = g_error_new_literal (g_quark_from_string ("Stamp"), 1, "Failed to handle internal response");
    webkit_uri_scheme_request_finish_error (request, error);
  } else {
    if (G_IS_SEEKABLE (stream)) {
      GSeekable *seekable = G_SEEKABLE (stream);
      g_seekable_seek (seekable, 0, G_SEEK_SET, NULL, NULL);
    }

    webkit_uri_scheme_request_finish (request, stream, -1, NULL);
  }
}

#define MAX_PIXELS 8 * 1024 * 1024

static void
on_get_page_size (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  StampWebView *self = STAMP_WEB_VIEW (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (WebKitUserMessage) response = NULL;
  GVariant *variant;
  gint32 width, height;

  response = webkit_web_view_send_message_to_page_finish (WEBKIT_WEB_VIEW (source), res, &error);
  if (error) {
    g_warning ("Could not send message to page: %s", error->message);
    return;
  }

  variant = webkit_user_message_get_parameters (response);
  g_variant_get (variant, "(uu)", &width, &height);

  if (width * height > MAX_PIXELS)
    height = floor (MAX_PIXELS / width);

  self->width_request = width;
  self->height_request = height;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SIZE_REQUEST]);
}

static void
request_page_size (StampWebView *self)
{
  WebKitUserMessage *message = webkit_user_message_new ("get-page-size", NULL);

  webkit_web_view_send_message_to_page (WEBKIT_WEB_VIEW (self), message, self->cancellable, on_get_page_size, self);
}

static void
request_page_size_timeout (gpointer user_data)
{
  StampWebView *self = STAMP_WEB_VIEW (user_data);

  request_page_size (self);
  self->page_size_timeout_handler = 0;
}

static void
collapse_quotes_in_page (WebKitWebView *web_view)
{
  g_autoptr (GBytes) data = g_resources_lookup_data ("/org/tabos/stamp/stamp-quote-collapse.js", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);

  webkit_web_view_evaluate_javascript (web_view, g_bytes_get_data (data, NULL), -1, NULL, NULL, NULL, NULL, NULL);
}

static void
on_load_changed (WebKitWebView   *web_view,
                 WebKitLoadEvent  load_event,
                 gpointer         user_data)
{
  StampWebView *self = STAMP_WEB_VIEW (user_data);

  g_signal_emit (self, signals[LOADED], 0, load_event == WEBKIT_LOAD_FINISHED);

  if (load_event == WEBKIT_LOAD_FINISHED) {
    self->loaded = TRUE;

    if (self->queued_body_content) {
      stamp_web_view_set_body_content (self, self->queued_body_content);
    }

    if (self->queued_load_images) {
      stamp_web_view_load_images (self);
    }

    webkit_web_view_evaluate_javascript (web_view,
                                         "var ed = document.getElementById('message-body'); if (ed) ed.focus();",
                                         -1, NULL, NULL, NULL, NULL, NULL);

    webkit_web_view_evaluate_javascript (
      web_view,
      "const style = document.createElement('style');"
      "style.innerHTML = `"

      "@media (prefers-color-scheme: dark) {"
      "  * {"
      "    background-color: transparent !important;"
      "    color: inherit !important;"
      "  }"
      "  html, body {"
      "    background:#3a3a3a !important;"
      "    color:#ffffff !important;"
      "  }"

      "  a {"
      "    color:#8ab4f8 !important;"
      "  }"
      "}"
      "`;"
      "document.head.appendChild(style);",
      -1, NULL, NULL, NULL, NULL, NULL
      );

    if (!self->queued_body_content) {
      collapse_quotes_in_page (web_view);

      g_clear_handle_id (&self->page_size_timeout_handler, g_source_remove);
      self->page_size_timeout_handler = g_timeout_add_once (150, request_page_size_timeout, self);
    }
  }
}

static gboolean
on_decide_policy (WebKitWebView            *web_view,
                  WebKitPolicyDecision     *decision,
                  WebKitPolicyDecisionType  type,
                  gpointer                  user_data)
{
  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION || type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
    WebKitNavigationPolicyDecision *navigation_decision;
    WebKitNavigationAction *navigation_action;
    WebKitNavigationType navigation_type;

    navigation_decision = WEBKIT_NAVIGATION_POLICY_DECISION (decision);
    navigation_action = webkit_navigation_policy_decision_get_navigation_action (navigation_decision);

    navigation_type = webkit_navigation_action_get_navigation_type (navigation_action);
    if (navigation_type == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
      WebKitURIRequest *request = webkit_navigation_action_get_request (navigation_action);
      const gchar *uri = webkit_uri_request_get_uri (request);
      g_autoptr (GError) error = NULL;

      if (!g_app_info_launch_default_for_uri (uri, NULL, &error)) {
        g_warning ("Could not launch URI '%s': %s", uri, error ? error->message : "unknown error");
      }
      webkit_policy_decision_ignore (decision);
      return GDK_EVENT_STOP;
    } else if (navigation_type == WEBKIT_NAVIGATION_TYPE_OTHER) {
      webkit_policy_decision_use (decision);
      return GDK_EVENT_STOP;
    }
  }

  webkit_policy_decision_ignore (decision);
  return GDK_EVENT_STOP;
}

static void
initialize_web_process_extensions (WebKitWebContext *web_context,
                                   gpointer          user_data)
{
  webkit_web_context_set_web_process_extensions_directory (web_context, STAMP_WEB_PROCESS_EXTENSIONS_DIR);
  webkit_web_context_set_web_process_extensions_initialization_user_data (web_context, g_variant_new_int32 (0));
}

static WebKitSettings *settings;

static gpointer
stamp_prefs_init (gpointer user_data)
{
  WebKitWebContext *web_context;
  g_autoptr (GStrvBuilder) builder = NULL;
  g_auto (GStrv) languages = NULL;

  settings = webkit_settings_new_with_settings ("allow-modal-dialogs", FALSE,
                                                "enable-fullscreen", FALSE,
                                                "enable-html5-database", FALSE,
                                                "enable-html5-local-storage", FALSE,
                                                "enable-javascript", TRUE,
                                                "enable-javascript-markup", FALSE,
                                                "enable-media-stream", FALSE,
                                                "enable-write-console-messages-to-stdout", TRUE,
                                                NULL);

  web_context = webkit_web_context_get_default ();
  webkit_web_context_set_spell_checking_enabled (web_context, TRUE);

  builder = g_strv_builder_new ();
  g_strv_builder_add (builder, "de_DE");
  languages = g_strv_builder_end (builder);

  webkit_web_context_set_spell_checking_languages (
    web_context,
    (const char * const *)languages
    );

  g_signal_connect (web_context, "initialize-web-process-extensions", G_CALLBACK (initialize_web_process_extensions), NULL);
  webkit_web_context_register_uri_scheme (web_context, "cid", on_cid_request, NULL, NULL);

  return settings;
}

static WebKitSettings *
stamp_get_webkit_settings (StampWebView *self)
{
  static GOnce once_init = G_ONCE_INIT;

  return g_once (&once_init, stamp_prefs_init, NULL);
}

static
void
on_key_released (GtkEventControllerKey *controller,
                 guint                  keyval,
                 guint                  keycode,
                 GdkModifierType        state,
                 gpointer               user_data)
{
  StampWebView *self = STAMP_WEB_VIEW (user_data);

  self->body_html_changed = TRUE;
}

static void
on_resize_script_message (WebKitUserContentManager *manager,
                          WebKitUserMessage        *message,
                          StampWebView             *self)
{
  g_clear_handle_id (&self->page_size_timeout_handler, g_source_remove);
  self->page_size_timeout_handler = g_timeout_add_once (150, request_page_size_timeout, self);
}

static void
stamp_webview_constructed (GObject *object)
{
  StampWebView *self = STAMP_WEB_VIEW (object);
  GtkEventController *controller;
  WebKitUserContentManager *content_manager;

  G_OBJECT_CLASS (stamp_webview_parent_class)->constructed (object);

  self->internal_resources = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->queued_body_content = NULL;

  content_manager = webkit_web_view_get_user_content_manager (WEBKIT_WEB_VIEW (self));
  g_signal_connect_object (content_manager, "script-message-received::stampResize", G_CALLBACK (on_resize_script_message), self, G_CONNECT_DEFAULT);
  webkit_user_content_manager_register_script_message_handler (content_manager, "stampResize", NULL);

  webkit_web_view_set_settings (WEBKIT_WEB_VIEW (self), stamp_get_webkit_settings (self));

  controller = gtk_event_controller_key_new ();
  g_signal_connect_object (G_OBJECT (controller), "key-released", G_CALLBACK (on_key_released), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);
  g_signal_connect_object (G_OBJECT (self), "load-changed", G_CALLBACK (on_load_changed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (G_OBJECT (self), "decide-policy", G_CALLBACK (on_decide_policy), self, G_CONNECT_DEFAULT);
}

static void
stamp_webview_image_load_blocked (StampWebView *self)
{
  g_signal_emit (self, signals[IMAGE_LOAD_BLOCKED], 0);
}

static gboolean
stamp_webview_user_message_received (WebKitWebView     *webview,
                                     WebKitUserMessage *message)
{
  StampWebView *self = STAMP_WEB_VIEW (webview);
  const gchar *name = webkit_user_message_get_name (message);

  if (g_strcmp0 (name, "image-load-blocked") == 0) {
    if (!self->queued_load_images)
      stamp_webview_image_load_blocked (self);

    return TRUE;
  } else if (g_strcmp0 (name, "selection-changed") == 0) {
    g_signal_emit (self, signals[SELECTION_CHANGED], 0);
  } else {
    g_critical ("%s: Unhandled message: %s", G_STRFUNC, name);
  }

  return FALSE;
}

static void
stamp_web_view_dispose (GObject *object)
{
  StampWebView *self = STAMP_WEB_VIEW (object);

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  g_clear_object (&self->cancellable);

  g_clear_handle_id (&self->page_size_timeout_handler, g_source_remove);

  g_clear_pointer (&self->queued_body_content, g_free);
  g_clear_pointer (&self->internal_resources, g_hash_table_unref);

  G_OBJECT_CLASS (stamp_webview_parent_class)->dispose (object);
}

static void
stamp_webview_class_init (StampWebViewClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  WebKitWebViewClass *widget_class = WEBKIT_WEB_VIEW_CLASS (klass);

  gobject_class->constructed = stamp_webview_constructed;
  gobject_class->get_property = stamp_webview_get_property;
  gobject_class->set_property = stamp_web_view_set_property;
  gobject_class->dispose = stamp_web_view_dispose;

  widget_class->user_message_received = stamp_webview_user_message_received;

  signals[IMAGE_LOAD_BLOCKED] = g_signal_new ("image-load-blocked", G_OBJECT_CLASS_TYPE (klass),
                                              G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                              0, NULL, NULL, NULL,
                                              G_TYPE_NONE,
                                              0);
  signals[SELECTION_CHANGED] = g_signal_new ("selection-changed", G_OBJECT_CLASS_TYPE (klass),
                                             G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                             0, NULL, NULL, NULL,
                                             G_TYPE_NONE,
                                             0);
  signals[LOADED] = g_signal_new ("loaded", G_OBJECT_CLASS_TYPE (klass),
                                  G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                  0, NULL, NULL, NULL,
                                  G_TYPE_NONE,
                                  1, G_TYPE_BOOLEAN);

  properties[PROP_SIZE_REQUEST] = g_param_spec_boolean ("size-request",
                                                        NULL,
                                                        NULL,
                                                        FALSE,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (properties), properties);
}

static void
stamp_webview_init (StampWebView *self)
{
}

StampWebView *
stamp_webview_new (void)
{
  if (!shared_process_anchor) {
    stamp_get_webkit_settings (NULL);
    shared_process_anchor = g_object_ref_sink (g_object_new (WEBKIT_TYPE_WEB_VIEW, "settings", settings, NULL));
  }

  return g_object_new (STAMP_TYPE_WEBVIEW, "related-view", shared_process_anchor, NULL);
}

void
stamp_webview_load_html (StampWebView *self,
                         gchar        *content)
{
  if (content)
    webkit_web_view_load_html (WEBKIT_WEB_VIEW (self), content, NULL);
}

void
stamp_webview_load_plain_text (StampWebView *self,
                               gchar        *content)
{
  if (content) {
    g_autoptr (GBytes) template = g_resources_lookup_data ("/org/tabos/stamp/blank-message-template.html", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);

    webkit_web_view_load_html (WEBKIT_WEB_VIEW (self), g_bytes_get_data (template, NULL), NULL);

    stamp_web_view_set_body_content (self, content);
  }
}

void
stamp_webview_add_internal_resource (StampWebView *self,
                                     const gchar  *id,
                                     GInputStream *stream)
{
  g_hash_table_insert (self->internal_resources, g_strdup (id), g_object_ref (stream));
}

void
stamp_webview_get_body_html (StampWebView        *self,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  WebKitUserMessage *message = webkit_user_message_new ("get-body-html", g_variant_new_boolean (TRUE));

  webkit_web_view_send_message_to_page (WEBKIT_WEB_VIEW (self), message, cancellable, callback, user_data);
}

gchar *
stamp_webview_get_body_html_finish (StampWebView  *self,
                                    GAsyncResult  *res,
                                    gchar        **out_plain_text,
                                    GError       **error)
{
  g_autoptr (GError) local_error = NULL;
  g_autoptr (WebKitUserMessage) response = webkit_web_view_send_message_to_page_finish (WEBKIT_WEB_VIEW (self), res, &local_error);
  GVariant *parameters;
  const gchar *html;
  const gchar *plain;

  if (local_error) {
    g_propagate_error (error, g_steal_pointer (&local_error));
    return NULL;
  }

  if (!response) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "No response from WebKit page");
    return NULL;
  }

  parameters = webkit_user_message_get_parameters (response);
  if (!parameters) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "WebKit response carries no parameters");
    return NULL;
  }

  g_variant_get (parameters, "(&s&s)", &html, &plain);

  if (out_plain_text)
    *out_plain_text = g_strdup (plain);

  return g_strdup (html);
}

static void
on_set_body_html (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  StampWebView *self = STAMP_WEB_VIEW (source);
  WebKitWebView *web_view = WEBKIT_WEB_VIEW (source);

  webkit_web_view_send_message_to_page_finish (web_view, res, NULL);

  collapse_quotes_in_page (web_view);

  g_clear_handle_id (&self->page_size_timeout_handler, g_source_remove);
  self->page_size_timeout_handler = g_timeout_add_once (150, request_page_size_timeout, self);

  gtk_widget_grab_focus (GTK_WIDGET (self));
}

void
stamp_web_view_set_body_content (StampWebView *self,
                                 gchar        *content)
{
  if (self->loaded) {
    WebKitUserMessage *message = webkit_user_message_new ("set-body-html", g_variant_new_string (content));

    if (self->cancellable)
      g_cancellable_cancel (self->cancellable);

    g_clear_object (&self->cancellable);

    self->cancellable = g_cancellable_new ();
    webkit_web_view_send_message_to_page (WEBKIT_WEB_VIEW (self), message, self->cancellable, on_set_body_html, self);
  } else {
    if (self->queued_body_content == content)
      return;

    g_set_str (&self->queued_body_content, content);
  }
}

static void
on_set_image_loading_enabled (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  StampWebView *self = STAMP_WEB_VIEW (user_data);
  g_autoptr (GError) error = NULL;

  webkit_web_view_send_message_to_page_finish (WEBKIT_WEB_VIEW (source), res, &error);
  if (error) {
    g_warning ("%s: Could not enable image loading: %s", G_STRFUNC, error->message);
    return;
  }

  /* Request page size again so that it can be rescaled */
  g_clear_handle_id (&self->page_size_timeout_handler, g_source_remove);
  self->page_size_timeout_handler = g_timeout_add_once (150, request_page_size_timeout, user_data);
}

void
stamp_web_view_load_images (StampWebView *self)
{
  if (self->loaded) {
    WebKitUserMessage *message = webkit_user_message_new ("set-image-loading-enabled", g_variant_new_boolean (TRUE));

    webkit_web_view_send_message_to_page (WEBKIT_WEB_VIEW (self), message, self->cancellable, on_set_image_loading_enabled, self);
  } else {
    self->queued_load_images = TRUE;
  }
}

void
stamp_web_view_query_command_state (StampWebView        *self,
                                    const gchar         *command,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  WebKitUserMessage *message = webkit_user_message_new ("query-command-state", g_variant_new_string (command));

  webkit_web_view_send_message_to_page (WEBKIT_WEB_VIEW (self), message, cancellable, callback, user_data);
}

gboolean
stamp_web_view_query_command_state_finish (GObject       *source,
                                           GAsyncResult  *res,
                                           GError       **error)
{
  g_autoptr (GError) local_error = NULL;
  WebKitWebView *web_view = WEBKIT_WEB_VIEW (source);
  g_autoptr (WebKitUserMessage) response = NULL;
  GVariant *parameters;

  response = webkit_web_view_send_message_to_page_finish (web_view, res, &local_error);
  if (local_error) {
    g_warning ("Could not query command state: %s", local_error->message);
    g_propagate_error (error, local_error);
    return FALSE;
  }

  parameters = webkit_user_message_get_parameters (response);
  return g_variant_get_boolean (parameters);
}

void
stamp_web_view_execute_editor_command (StampWebView *self,
                                       const gchar  *command,
                                       const gchar  *argument)
{
  g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
  g_auto (GStrv) arguments = NULL;
  WebKitUserMessage *message;

  g_strv_builder_add (builder, command);
  g_strv_builder_add (builder, argument);

  arguments = g_strv_builder_end (builder);
  message = webkit_user_message_new ("execute-editor-command", g_variant_new_strv ((const char * const *)arguments, 2));

  webkit_web_view_send_message_to_page (WEBKIT_WEB_VIEW (self), message, self->cancellable, NULL, NULL);
}

void
stamp_web_view_get_size (StampWebView *self,
                         gint32       *width,
                         gint32       *height)
{
  *width = self->width_request;
  *height = self->height_request;
}

gboolean
stamp_web_view_changed (StampWebView *self)
{
  return self->body_html_changed;
}

void
stamp_webview_set_editable (StampWebView *self)
{
  webkit_web_view_set_editable (WEBKIT_WEB_VIEW (self), TRUE);
}

void
stamp_webview_copy_resources (StampWebView *src,
                              StampWebView *dst)
{
  dst->internal_resources = g_hash_table_ref (src->internal_resources);
}

void
stamp_webview_set_cid_handler (StampWebView     *self,
                               cid_handler_func  cid_handler,
                               gpointer          user_data)
{
  self->cid_handler = cid_handler;
  self->cid_handler_user_data = user_data;
}
