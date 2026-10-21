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

#include "extension.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

void webkit_web_process_extension_initialize (WebKitWebProcessExtension *extension);

#define SHOW_IMAGES_KEY "stamp-show-images"

static gboolean
on_user_message_received (WebKitWebPage     *page,
                          WebKitUserMessage *message)
{
  const gchar *name = webkit_user_message_get_name (message);
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  WebKitFrame *frame = webkit_web_page_get_main_frame (page);
  G_GNUC_END_IGNORE_DEPRECATIONS
  JSCContext *jsc_context = webkit_frame_get_js_context (frame);

  if (g_strcmp0 (name, "get-body-html") == 0) {
    GVariant *parameters = webkit_user_message_get_parameters (message);
    WebKitUserMessage *reply;
    g_autoptr (JSCValue) html_value = NULL;
    g_autoptr (JSCValue) plain_value = NULL;

    if (g_variant_get_boolean (parameters)) {
      g_autoptr (JSCValue) flush = jsc_context_evaluate (jsc_context, "", -1);
      (void)flush;
    }

    html_value = jsc_context_evaluate (jsc_context, "document.querySelector('body').innerHTML;", -1);
    plain_value = jsc_context_evaluate (jsc_context, "document.querySelector('body').innerText;", -1);

    reply = webkit_user_message_new ("get-body-html",
                                     g_variant_new ("(ss)",
                                                    jsc_value_to_string (html_value),
                                                    jsc_value_to_string (plain_value)));
    webkit_user_message_send_reply (message, reply);
  } else if (g_strcmp0 (name, "get-page-height") == 0) {
    g_autoptr (JSCValue) value = NULL;
    WebKitUserMessage *reply;

    value = jsc_context_evaluate (jsc_context,
                                  "Math.max(document.body.scrollHeight, document.body.offsetHeight, document.documentElement.clientHeight, document.documentElement.scrollHeight, document.documentElement.offsetHeight);", -1);
    reply = webkit_user_message_new ("get-page-height", g_variant_new_int32 (jsc_value_to_int32 (value)));
    webkit_user_message_send_reply (message, reply);
  } else if (g_strcmp0 (name, "get-page-size") == 0) {
    g_autoptr (JSCValue) width = NULL;
    g_autoptr (JSCValue) height = NULL;
    WebKitUserMessage *reply;

    width = jsc_context_evaluate (jsc_context,
                                  "Math.max(document.body.scrollWidth, document.body.offsetWidth, document.documentElement.clientWidth, document.documentElement.scrollWidth, document.documentElement.offsetWidth);", -1);

    height = jsc_context_evaluate (jsc_context,
                                   "Math.max(document.body.scrollHeight, document.body.offsetHeight, document.documentElement.clientHeight, document.documentElement.scrollHeight, document.documentElement.offsetHeight);", -1);

    reply = webkit_user_message_new ("get-page-size", g_variant_new ("(uu)", jsc_value_to_int32 (width), jsc_value_to_int32 (height)));
    webkit_user_message_send_reply (message, reply);
  } else if (g_strcmp0 (name, "set-body-html") == 0) {
    GVariant *parameters = webkit_user_message_get_parameters (message);
    const gchar *body_html = g_variant_get_string (parameters, NULL);
    g_autoptr (JSCValue) body = NULL;
    WebKitUserMessage *reply;

    body = jsc_context_evaluate (jsc_context, "document.querySelector('#message-body')", -1);
    jsc_value_object_set_property (body, "innerHTML", jsc_value_new_string (jsc_context, body_html));

    reply = webkit_user_message_new ("set-body-html", NULL);
    webkit_user_message_send_reply (message, reply);
  } else if (g_strcmp0 (name, "set-image-loading-enabled") == 0) {
    GVariant *parameters = webkit_user_message_get_parameters (message);
    g_autoptr (JSCValue) value = NULL;
    gboolean enabled = g_variant_get_boolean (parameters);

    g_object_set_data (G_OBJECT (page), SHOW_IMAGES_KEY, GINT_TO_POINTER (enabled));
    if (enabled) {
      JSCException *exception;

      value = jsc_context_evaluate (jsc_context,
                                    "var images = document.images;" \
                                    "for(var i = 0; i < images.length; i++) {" \
                                    "  images[i].src = images[i].src" \
                                    "}",
                                    -1);
      exception = jsc_context_get_exception (jsc_context);
      if (!value || exception) {
        g_warning ("JavaScript error: %s", jsc_exception_get_message (exception));
        jsc_context_clear_exception (jsc_context);
      } else {
        WebKitUserMessage *reply;

        reply = webkit_user_message_new ("set-image-loading-enabled", g_variant_new_boolean (jsc_value_to_boolean (value)));
        webkit_user_message_send_reply (message, reply);
      }
    }
  } else if (g_strcmp0 (name, "query-command-state") == 0) {
    GVariant *parameters = webkit_user_message_get_parameters (message);
    g_autoptr (JSCValue) document = NULL;
    g_autoptr (JSCValue) value = NULL;
    const gchar *command = g_variant_get_string (parameters, NULL);
    JSCValue *param1 = jsc_value_new_string (jsc_context, command);
    JSCValue *params[] = { param1 };

    document = jsc_context_evaluate (jsc_context, "document", -1);
    value = jsc_value_object_invoke_methodv (document, "queryCommandState", 1, params);
    if (jsc_value_is_boolean (value)) {
      WebKitUserMessage *reply;

      reply = webkit_user_message_new ("query-command-state", g_variant_new_boolean (jsc_value_to_boolean (value)));
      webkit_user_message_send_reply (message, reply);
    } else {
      g_warning ("%s: Unknown feedback: %s", G_STRFUNC, jsc_value_to_string (value));
    }
  } else if (g_strcmp0 (name, "execute-editor-command") == 0) {
    GVariant *parameters = webkit_user_message_get_parameters (message);
    g_autoptr (JSCValue) document = NULL;
    g_autoptr (JSCValue) value = NULL;
    const gchar **arguments = g_variant_get_strv (parameters, NULL);
    JSCValue *param1 = jsc_value_new_string (jsc_context, arguments[0]);
    JSCValue *param2 = jsc_value_new_boolean (jsc_context, FALSE);
    JSCValue *param3 = jsc_value_new_string (jsc_context, arguments[1]);
    JSCValue *params[] = { param1, param2, param3 };

    document = jsc_context_evaluate (jsc_context, "document", -1);
    value = jsc_value_object_invoke_methodv (document, "execCommand", 3, params);
    if (!jsc_value_is_boolean (value)) {
      g_warning ("%s: Unknown feedback: %s", G_STRFUNC, jsc_value_to_string (value));
    }
  } else {
    g_warning ("%s: Unhandled page message: %s", G_STRFUNC, name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
on_send_request (WebKitWebPage     *web_page,
                 WebKitURIRequest  *request,
                 WebKitURIResponse *redirected_response,
                 gpointer           user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GUri) uri = NULL;
  WebKitUserMessage *message;

  uri = g_uri_parse (webkit_uri_request_get_uri (request), G_URI_FLAGS_NONE, &error);
  if (!uri) {
    g_warning ("Could not parse uri: %s", error->message);
    return GDK_EVENT_STOP;
  }

  if (g_strcmp0 (g_uri_get_scheme (uri), "cid") == 0 ||
      g_strcmp0 (g_uri_get_scheme (uri), "data") == 0 ||
      g_strcmp0 (g_uri_get_scheme (uri), "about") == 0) {
    return GDK_EVENT_PROPAGATE;
  }

  if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (web_page), SHOW_IMAGES_KEY)))
    return GDK_EVENT_PROPAGATE;

  message = webkit_user_message_new ("image-load-blocked", NULL);
  webkit_web_page_send_message_to_view (web_page, message, NULL, NULL, NULL);

  return GDK_EVENT_STOP;
}

static void
on_selection_changed (WebKitWebEditor *editor,
                      gpointer         user_data)
{
  WebKitUserMessage *message;

  message = webkit_user_message_new ("selection-changed", NULL);
  webkit_web_page_send_message_to_view (webkit_web_editor_get_page (editor), message, NULL, NULL, NULL);
}

static void
on_page_created (WebKitWebProcessExtension *extension,
                 WebKitWebPage             *page,
                 gpointer                   user_data)
{
  WebKitWebEditor *editor = webkit_web_page_get_editor (page);

  g_signal_connect (G_OBJECT (page), "user-message-received", G_CALLBACK (on_user_message_received), NULL);
  g_signal_connect (G_OBJECT (page), "send-request", G_CALLBACK (on_send_request), NULL);
  g_signal_connect (G_OBJECT (editor), "selection-changed", G_CALLBACK (on_selection_changed), NULL);
}

G_MODULE_EXPORT void
webkit_web_process_extension_initialize (WebKitWebProcessExtension *webkit_extension)
{
  g_signal_connect (G_OBJECT (webkit_extension), "page-created", G_CALLBACK (on_page_created), NULL);
}

static void __attribute__((destructor))
stamp_web_process_extension_shutdown (void)
{
}

#pragma GCC diagnostic pop
