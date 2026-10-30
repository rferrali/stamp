/*
 * Copyright 2026 Jan-Michael Brummer
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
#include "stamp-rich-text-editor.h"

#include <glib/gi18n.h>

struct _StampRichTextEditor {
  AdwBin parent_instance;

  AdwBin *webview_bin;
  AdwWrapBox *toolbar;
  StampWebView *web_view;
  GSimpleActionGroup *actions;
  GCancellable *cancellable;
  gchar *pending_html;
  gboolean document_loaded;
  gboolean applying_content;
  gboolean ready;
  gboolean disposed;
};

static void buildable_iface_init (GtkBuildableIface *iface);
static GtkBuildableIface *parent_buildable_iface;

G_DEFINE_FINAL_TYPE_WITH_CODE (StampRichTextEditor, stamp_rich_text_editor, ADW_TYPE_BIN, G_IMPLEMENT_INTERFACE (GTK_TYPE_BUILDABLE, buildable_iface_init));

enum { PROP_0, PROP_READY, N_PROPS };
static GParamSpec *properties[N_PROPS];
enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static const struct {
  const gchar *action;
  const gchar *command;
} formats[] = {
  { "bold", "bold" },
  { "italic", "italic" },
  { "underline", "underline" },
  { "strikethrough", "strikethrough" },
  { "bullet-list", "insertUnorderedList" },
  { "numbered-list", "insertOrderedList" },
};

static void
set_ready (StampRichTextEditor *self,
           gboolean             ready)
{
  self->ready = ready;
  gtk_widget_set_sensitive (GTK_WIDGET (self->toolbar), ready);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_READY]);
}

static void
execute_command (StampRichTextEditor *self,
                 const gchar         *command,
                 const gchar         *argument)
{
  g_autofree char *encoded = g_base64_encode ((const guchar *)argument, strlen (argument));
  g_autofree char *script = g_strdup_printf (
    "window.stampEditorExecute('%s', new TextDecoder('utf-8').decode(Uint8Array.from(atob('%s'), c => c.charCodeAt(0))));",
    command, encoded);

  if (!self->ready)
    return;

  gtk_widget_grab_focus (GTK_WIDGET (self->web_view));
  webkit_web_view_evaluate_javascript (WEBKIT_WEB_VIEW (self->web_view), script, -1,
                                       NULL, NULL, self->cancellable, NULL, NULL);
}

static void
on_format (GSimpleAction *action,
           GVariant      *parameter,
           gpointer       user_data)
{
  const gchar *name = g_action_get_name (G_ACTION (action));

  for (guint i = 0; i < G_N_ELEMENTS (formats); i++) {
    if (g_str_equal (name, formats[i].action)) {
      execute_command (STAMP_RICH_TEXT_EDITOR (user_data), formats[i].command, "");
      return;
    }
  }
}

static void
on_remove_format (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  StampRichTextEditor *self = STAMP_RICH_TEXT_EDITOR (user_data);

  execute_command (self, "removeFormat", "");
  execute_command (self, "unlink", "");
}

static void
on_link_response (AdwAlertDialog *dialog,
                  const gchar    *response,
                  gpointer        user_data)
{
  StampRichTextEditor *self = STAMP_RICH_TEXT_EDITOR (user_data);
  GtkWidget *box = adw_alert_dialog_get_extra_child (dialog);
  GtkWidget *text_entry = g_object_get_data (G_OBJECT (box), "text-entry");
  GtkWidget *url_entry = g_object_get_data (G_OBJECT (box), "url-entry");
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (text_entry));
  const gchar *url = gtk_editable_get_text (GTK_EDITABLE (url_entry));

  if (self->disposed || !g_str_equal (response, "insert") || !*url)
    return;

  if (*text) {
    g_autofree char *escaped_url = g_markup_escape_text (url, -1);
    g_autofree char *escaped_text = g_markup_escape_text (text, -1);
    g_autofree char *html = g_strdup_printf ("<a href=\"%s\">%s</a>", escaped_url, escaped_text);

    execute_command (self, "insertHTML", html);
  } else {
    execute_command (self, "createLink", url);
  }
}

static void
on_insert_link (GSimpleAction *action,
                GVariant      *parameter,
                gpointer       user_data)
{
  StampRichTextEditor *self = STAMP_RICH_TEXT_EDITOR (user_data);
  AdwDialog *dialog = adw_alert_dialog_new (_("Insert Link"), NULL);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *text_entry = gtk_entry_new ();
  GtkWidget *url_entry = gtk_entry_new ();

  gtk_entry_set_placeholder_text (GTK_ENTRY (text_entry), _("Text"));
  gtk_entry_set_placeholder_text (GTK_ENTRY (url_entry), _("URL"));
  gtk_box_append (GTK_BOX (box), text_entry);
  gtk_box_append (GTK_BOX (box), url_entry);

  g_object_set_data (G_OBJECT (box), "text-entry", text_entry);
  g_object_set_data (G_OBJECT (box), "url-entry", url_entry);

  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), box);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", _("_Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "insert", _("_Insert"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "insert", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "insert");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_link_response), self, G_CONNECT_DEFAULT);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_history (GSimpleAction *action,
            GVariant      *parameter,
            gpointer       user_data)
{
  execute_command (STAMP_RICH_TEXT_EDITOR (user_data), g_action_get_name (G_ACTION (action)), "");
}

static const GActionEntry editor_actions[] = {
  { "bold", on_format, NULL, "false" },
  { "italic", on_format, NULL, "false" },
  { "underline", on_format, NULL, "false" },
  { "strikethrough", on_format, NULL, "false" },
  { "bullet-list", on_format, NULL, "false" },
  { "numbered-list", on_format, NULL, "false" },
  { "insert-link", on_insert_link },
  { "remove-format", on_remove_format },
  { "undo", on_history },
  { "redo", on_history },
};

static gboolean
on_context_menu (WebKitWebView       *view,
                 WebKitContextMenu   *menu,
                 WebKitHitTestResult *hit_test,
                 gpointer             user_data)
{
  StampRichTextEditor *self = STAMP_RICH_TEXT_EDITOR (user_data);
  WebKitEditorState *state = webkit_web_view_get_editor_state (view);
  GAction *undo = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "undo");
  GAction *redo = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "redo");

  g_simple_action_set_enabled (G_SIMPLE_ACTION (undo), webkit_editor_state_is_undo_available (state));
  g_simple_action_set_enabled (G_SIMPLE_ACTION (redo), webkit_editor_state_is_redo_available (state));
  webkit_context_menu_prepend (menu, webkit_context_menu_item_new_separator ());
  webkit_context_menu_prepend (menu, webkit_context_menu_item_new_from_gaction (redo, _("Redo"), NULL));
  webkit_context_menu_prepend (menu, webkit_context_menu_item_new_from_gaction (undo, _("Undo"), NULL));
  return FALSE;
}

static void
on_changed (WebKitUserContentManager *manager,
            JSCValue                 *value,
            StampRichTextEditor      *self)
{
  if (!self->disposed && self->ready)
    g_signal_emit (self, signals[CHANGED], 0);
}

static void
on_format_state (WebKitUserContentManager *manager,
                 JSCValue                 *value,
                 StampRichTextEditor      *self)
{
  g_autoptr (JSCValue) undo = jsc_value_object_get_property_at_index (value, G_N_ELEMENTS (formats));
  g_autoptr (JSCValue) redo = jsc_value_object_get_property_at_index (value, G_N_ELEMENTS (formats) + 1);

  if (self->disposed || !jsc_value_is_array (value))
    return;

  for (guint i = 0; i < G_N_ELEMENTS (formats); i++) {
    g_autoptr (JSCValue) state = jsc_value_object_get_property_at_index (value, i);
    GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), formats[i].action);

    g_simple_action_set_state (G_SIMPLE_ACTION (action), g_variant_new_boolean (jsc_value_to_boolean (state)));
  }

  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), "undo")), jsc_value_to_boolean (undo));
  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), "redo")), jsc_value_to_boolean (redo));
}

static void apply_pending_html (StampRichTextEditor *self);

static void
on_content_applied (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (StampRichTextEditor) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JSCValue) value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (source), result, &error);

  if (self->disposed)
    return;

  self->applying_content = FALSE;
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Could not set editor content: %s", error->message);
    return;
  }

  if (self->pending_html)
    apply_pending_html (self);
  else
    set_ready (self, TRUE);
}

static void
apply_pending_html (StampRichTextEditor *self)
{
  g_autofree char *html = g_steal_pointer (&self->pending_html);
  g_autofree char *encoded = g_base64_encode ((const guchar *)html, strlen (html));
  g_autofree char *script = g_strdup_printf (
    "(() => {"
    "const html = new TextDecoder('utf-8').decode(Uint8Array.from(atob('%s'), c => c.charCodeAt(0)));"
    "const doc = new DOMParser().parseFromString(html, 'text/html');"
    "const content = doc.getElementById('message-body') || doc.body;"
    "document.getElementById('message-body').innerHTML = content.innerHTML;"
    "window.stampEditorResetSelection();"
    "})()", encoded);

  self->applying_content = TRUE;
  set_ready (self, FALSE);

  webkit_web_view_evaluate_javascript (WEBKIT_WEB_VIEW (self->web_view), script, -1,
                                       NULL, NULL, self->cancellable, on_content_applied, g_object_ref (self));
}

static void
on_load_changed (WebKitWebView   *view,
                 WebKitLoadEvent  event,
                 gpointer         user_data)
{
  StampRichTextEditor *self = STAMP_RICH_TEXT_EDITOR (user_data);

  if (event == WEBKIT_LOAD_STARTED) {
    self->document_loaded = FALSE;
    set_ready (self, FALSE);
  } else if (event == WEBKIT_LOAD_FINISHED) {
    self->document_loaded = TRUE;
    if (self->pending_html)
      apply_pending_html (self);
    else
      set_ready (self, TRUE);
  }
}

static void
buildable_add_child (GtkBuildable *buildable,
                     GtkBuilder   *builder,
                     GObject      *child,
                     const gchar  *type)
{
  if (g_strcmp0 (type, "toolbar") == 0 && GTK_IS_WIDGET (child))
    stamp_rich_text_editor_append_toolbar (STAMP_RICH_TEXT_EDITOR (buildable), GTK_WIDGET (child));
  else
    parent_buildable_iface->add_child (buildable, builder, child, type);
}

static void
buildable_iface_init (GtkBuildableIface *iface)
{
  parent_buildable_iface = g_type_interface_peek_parent (iface);
  iface->add_child = buildable_add_child;
}

static void
stamp_rich_text_editor_get_property (GObject    *object,
                                     guint       property_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  if (property_id == PROP_READY)
    g_value_set_boolean (value, STAMP_RICH_TEXT_EDITOR (object)->ready);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
stamp_rich_text_editor_dispose (GObject *object)
{
  StampRichTextEditor *self = STAMP_RICH_TEXT_EDITOR (object);

  self->disposed = TRUE;
  self->ready = FALSE;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->actions);
  g_clear_pointer (&self->pending_html, g_free);

  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_RICH_TEXT_EDITOR);

  G_OBJECT_CLASS (stamp_rich_text_editor_parent_class)->dispose (object);
}

static void
stamp_rich_text_editor_class_init (StampRichTextEditorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = stamp_rich_text_editor_get_property;
  object_class->dispose = stamp_rich_text_editor_dispose;

  properties[PROP_READY] = g_param_spec_boolean ("ready", NULL, NULL, FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                   0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/stamp-rich-text-editor.ui");
  gtk_widget_class_bind_template_child (widget_class, StampRichTextEditor, webview_bin);
  gtk_widget_class_bind_template_child (widget_class, StampRichTextEditor, toolbar);
}

static void
stamp_rich_text_editor_init (StampRichTextEditor *self)
{
  g_autoptr (GBytes) script = NULL;
  g_autoptr (GBytes) document = NULL;
  g_autoptr (WebKitUserScript) user_script = NULL;
  WebKitUserContentManager *manager;

  self->cancellable = g_cancellable_new ();
  gtk_widget_init_template (GTK_WIDGET (self));
  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions), editor_actions, G_N_ELEMENTS (editor_actions), self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "editor", G_ACTION_GROUP (self->actions));

  self->web_view = stamp_webview_new ();
  stamp_webview_set_editable (self->web_view);
  g_signal_connect_object (self->web_view, "context-menu", G_CALLBACK (on_context_menu), self, G_CONNECT_DEFAULT);
  gtk_widget_set_focusable (GTK_WIDGET (self->web_view), TRUE);
  adw_bin_set_child (self->webview_bin, GTK_WIDGET (self->web_view));

  manager = webkit_web_view_get_user_content_manager (WEBKIT_WEB_VIEW (self->web_view));

  g_signal_connect_object (manager, "script-message-received::stampEditorChanged", G_CALLBACK (on_changed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (manager, "script-message-received::stampEditorState", G_CALLBACK (on_format_state), self, G_CONNECT_DEFAULT);
  webkit_user_content_manager_register_script_message_handler (manager, "stampEditorChanged", NULL);
  webkit_user_content_manager_register_script_message_handler (manager, "stampEditorState", NULL);
  script = g_resources_lookup_data ("/org/tabos/stamp/stamp-rich-text-editor.js", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  user_script = webkit_user_script_new (g_bytes_get_data (script, NULL), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
  webkit_user_content_manager_add_script (manager, user_script);

  g_signal_connect_object (self->web_view, "load-changed", G_CALLBACK (on_load_changed), self, G_CONNECT_DEFAULT);
  set_ready (self, FALSE);
  document = g_resources_lookup_data ("/org/tabos/stamp/blank-message-template.html", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);

  webkit_web_view_load_html (WEBKIT_WEB_VIEW (self->web_view), g_bytes_get_data (document, NULL), NULL);
}

GtkWidget *
stamp_rich_text_editor_new (void)
{
  return g_object_new (STAMP_TYPE_RICH_TEXT_EDITOR, NULL);
}

StampWebView *
stamp_rich_text_editor_get_web_view (StampRichTextEditor *self)
{
  g_return_val_if_fail (STAMP_IS_RICH_TEXT_EDITOR (self), NULL);
  return self->web_view;
}

AdwWrapBox *
stamp_rich_text_editor_get_toolbar (StampRichTextEditor *self)
{
  g_return_val_if_fail (STAMP_IS_RICH_TEXT_EDITOR (self), NULL);
  return self->toolbar;
}

void
stamp_rich_text_editor_append_toolbar (StampRichTextEditor *self,
                                       GtkWidget           *child)
{
  g_return_if_fail (STAMP_IS_RICH_TEXT_EDITOR (self));
  g_return_if_fail (GTK_IS_WIDGET (child));
  adw_wrap_box_append (self->toolbar, child);
}

void
stamp_rich_text_editor_set_html (StampRichTextEditor *self,
                                 const gchar         *html)
{
  g_return_if_fail (STAMP_IS_RICH_TEXT_EDITOR (self));
  g_set_str (&self->pending_html, html ? html : "");
  set_ready (self, FALSE);

  if (self->document_loaded && !self->applying_content)
    apply_pending_html (self);
}

void
stamp_rich_text_editor_set_plain_text (StampRichTextEditor *self,
                                       const gchar         *text)
{
  g_autofree char *escaped = g_markup_escape_text (text ? text : "", -1);
  g_auto (GStrv) lines = g_strsplit (escaped, "\n", -1);
  g_autofree char *html = g_strjoinv ("<br>", lines);

  stamp_rich_text_editor_set_html (self, html);
}

typedef struct {
  gchar *html;
  gchar *plain;
} EditorContent;

static void
editor_content_free (gpointer data)
{
  EditorContent *content = data;
  g_free (content->html);
  g_free (content->plain);
  g_free (content);
}

static void
on_get_html (GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JSCValue) value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (source), result, &error);
  g_autoptr (JSCValue) html = NULL;
  g_autoptr (JSCValue) plain = NULL;
  EditorContent *content;

  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  if (!value || !jsc_value_is_array (value)) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "No editor content returned");
    return;
  }

  html = jsc_value_object_get_property_at_index (value, 0);
  plain = jsc_value_object_get_property_at_index (value, 1);
  content = g_new0 (EditorContent, 1);
  content->html = jsc_value_to_string (html);
  content->plain = jsc_value_to_string (plain);
  g_task_return_pointer (task, content, editor_content_free);
}

void
stamp_rich_text_editor_get_html_async (StampRichTextEditor *self,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (STAMP_IS_RICH_TEXT_EDITOR (self));
  task = g_task_new (self, cancellable, callback, user_data);

  if (!self->ready) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING, "Editor is still loading");
    g_object_unref (task);
    return;
  }

  webkit_web_view_evaluate_javascript (WEBKIT_WEB_VIEW (self->web_view),
                                       "(() => { const ed = document.getElementById('message-body'); return [ed.innerHTML, ed.innerText]; })()",
                                       -1, NULL, NULL, cancellable, on_get_html, task);
}

gchar *
stamp_rich_text_editor_get_html_finish (StampRichTextEditor  *self,
                                        GAsyncResult         *result,
                                        gchar               **out_plain_text,
                                        GError              **error)
{
  EditorContent *content;
  gchar *html;

  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  content = g_task_propagate_pointer (G_TASK (result), error);
  if (!content)
    return NULL;

  html = g_steal_pointer (&content->html);
  if (out_plain_text)
    *out_plain_text = g_steal_pointer (&content->plain);

  editor_content_free (content);

  return html;
}
