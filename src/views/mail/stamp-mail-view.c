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

#include "stamp-mail-view.h"

#include <glib/gi18n.h>

#include "stamp-collection-item.h"
#include "stamp-conversation-list.h"
#include "stamp-folder-list.h"
#include "stamp-header-bar.h"
#include "stamp-message-list.h"
#include "stamp-scheduler.h"

struct _StampMailView {
  AdwBreakpointBin parent_instance;

  AdwMultiLayoutView *mail_layout;
  AdwOverlaySplitView *tablet_osv;
  AdwOverlaySplitView *mobile_osv;
  AdwNavigationView *mobile_nav;
  StampFolderList *folder_list;
  StampMessageList *message_list;
  StampConversationList *conversation_list;
  GtkPaned *desktop_paned;
  GtkPaned *tablet_paned;
  AdwToastOverlay *toast_overlay;
  AdwToast *trash_toast;

  GtkWidget *stack;

  GSimpleActionGroup *actions;
  StampAccount *account;

  gint saved_paned_pos;
  guint load_folder_handler;
};

G_DEFINE_FINAL_TYPE (StampMailView, stamp_mail_view, ADW_TYPE_BREAKPOINT_BIN);

#define SIDEBAR_CLOSE_ANIMATION_MS 300

typedef enum {
  PROP_STACK = 1,
} StampMailViewProps;

static GParamSpec *obj_properties[PROP_STACK + 1];

static AdwOverlaySplitView *
get_current_osv (StampMailView *self)
{
  const gchar *layout = adw_multi_layout_view_get_layout_name (self->mail_layout);

  if (g_strcmp0 (layout, "tablet") == 0)
    return self->tablet_osv;

  if (g_strcmp0 (layout, "mobile") == 0)
    return self->mobile_osv;

  return NULL;
}

static void
on_details_hidden (AdwNavigationPage *page,
                   gpointer           user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  stamp_conversation_list_unselect (self->conversation_list);
}

static void
on_sidebar_visibility_changed (StampMailView *self)
{
  AdwOverlaySplitView *osv = get_current_osv (self);
  GtkToggleButton *toggle_button;
  gboolean shown;

  if (!osv)
    return;

  shown = adw_overlay_split_view_get_show_sidebar (osv);
  toggle_button = stamp_conversation_list_get_sidebar_button (self->conversation_list);
  if (gtk_toggle_button_get_active (toggle_button) != shown)
    gtk_toggle_button_set_active (toggle_button, shown);
}

static gboolean
close_overlay_sidebar (StampMailView *self)
{
  AdwOverlaySplitView *osv = get_current_osv (self);

  if (!osv || !adw_overlay_split_view_get_show_sidebar (osv))
    return FALSE;

  adw_overlay_split_view_set_show_sidebar (osv, FALSE);
  gtk_toggle_button_set_active (stamp_conversation_list_get_sidebar_button (self->conversation_list), FALSE);

  return TRUE;
}

typedef struct {
  StampMailView *self;
  StampAccount *account;
  gchar *full_name;
} LoadFolderData;

static void
load_folder_data_free (gpointer user_data)
{
  LoadFolderData *data = user_data;

  g_object_unref (data->account);
  g_clear_pointer (&data->full_name, g_free);
  g_free (data);
}

static gboolean
load_folder_idle (gpointer user_data)
{
  LoadFolderData *data = user_data;
  StampMailView *self = data->self;

  g_set_object (&self->account, data->account);
  stamp_conversation_list_load_folder (self->conversation_list, data->account, data->full_name);

  self->load_folder_handler = 0;
  return G_SOURCE_REMOVE;
}

static void
on_folder_selected (GtkWidget    *object,
                    StampAccount *account,
                    gchar        *full_name,
                    gpointer      user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);
  LoadFolderData *data;

  data = g_new0 (LoadFolderData, 1);
  data->self = self;
  data->account = g_object_ref (account);
  data->full_name = g_strdup (full_name);

  g_clear_handle_id (&self->load_folder_handler, g_source_remove);

  if (close_overlay_sidebar (self))
    self->load_folder_handler = g_timeout_add_full (G_PRIORITY_DEFAULT, SIDEBAR_CLOSE_ANIMATION_MS, load_folder_idle, data, load_folder_data_free);
  else
    self->load_folder_handler = g_idle_add_full (G_PRIORITY_DEFAULT, load_folder_idle, data, load_folder_data_free);

  stamp_message_list_set_conversation (self->message_list, self->account, NULL);
}

typedef struct {
  StampMailView *self;
  StampCollectionItem *collection;
} LoadCollectionData;

static void
load_collection_data_free (gpointer user_data)
{
  LoadCollectionData *data = user_data;

  g_object_unref (data->collection);
  g_free (data);
}

static gboolean
load_collection_idle (gpointer user_data)
{
  LoadCollectionData *data = user_data;
  StampMailView *self = data->self;

  stamp_conversation_list_load_collection (self->conversation_list, data->collection);

  self->load_folder_handler = 0;
  return G_SOURCE_REMOVE;
}

static void
on_collection_selected (GtkWidget           *object,
                        StampCollectionItem *collection,
                        gpointer             user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);
  LoadCollectionData *data;

  g_clear_handle_id (&self->load_folder_handler, g_source_remove);

  data = g_new0 (LoadCollectionData, 1);
  data->self = self;
  data->collection = g_object_ref (collection);

  if (close_overlay_sidebar (self))
    self->load_folder_handler = g_timeout_add_full (G_PRIORITY_DEFAULT, SIDEBAR_CLOSE_ANIMATION_MS, load_collection_idle, data, load_collection_data_free);
  else
    self->load_folder_handler = g_idle_add_full (G_PRIORITY_DEFAULT, load_collection_idle, data, load_collection_data_free);

  g_set_object (&self->account, NULL);
  stamp_message_list_set_conversation (self->message_list, NULL, NULL);
}

static void
on_conversation_selected (GtkWidget *object,
                          gpointer   thread_node,
                          gpointer   user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  if (thread_node) {
    const gchar *layout = adw_multi_layout_view_get_layout_name (self->mail_layout);

    if (g_strcmp0 (layout, "mobile") == 0) {
      AdwNavigationPage *visible = adw_navigation_view_get_visible_page (self->mobile_nav);
      const gchar *tag = adw_navigation_page_get_tag (visible);

      if (g_strcmp0 (tag, "content") != 0)
        adw_navigation_view_push_by_tag (self->mobile_nav, "content");
    }
  }

  stamp_message_list_set_conversation (self->message_list,
                                       stamp_conversation_list_get_selected_account (self->conversation_list),
                                       (CamelFolderThreadNode *)thread_node);
}

static gboolean
is_mobile_view (StampMailView *self);

static void
on_trash_undo (AdwToast *toast,
               gpointer  user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  if (toast != self->trash_toast)
    return;

  stamp_conversation_list_undo_trash (self->conversation_list);
  g_clear_object (&self->trash_toast);
}

static void
on_trash_dismissed (AdwToast *toast,
                    gpointer  user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  if (toast != self->trash_toast)
    return;

  stamp_conversation_list_finalize_trash (self->conversation_list, !is_mobile_view (self));
  g_clear_object (&self->trash_toast);
}

static void
trash (StampMailView         *self,
       StampConversationItem *item)
{
  if (self->trash_toast) {
    g_autoptr (AdwToast) previous_toast = g_steal_pointer (&self->trash_toast);

    stamp_conversation_list_commit_trash (self->conversation_list);
    adw_toast_dismiss (previous_toast);
  }

  if (stamp_conversation_list_trash (self->conversation_list, item)) {
    AdwToast *toast;

    toast = adw_toast_new (_("Conversations moved to trash"));
    adw_toast_set_button_label (toast, _("_Undo"));
    adw_toast_set_timeout (toast, 5);
    g_signal_connect (toast, "button-clicked", G_CALLBACK (on_trash_undo), self);
    g_signal_connect (toast, "dismissed", G_CALLBACK (on_trash_dismissed), self);
    self->trash_toast = g_object_ref (toast);
    adw_toast_overlay_add_toast (self->toast_overlay, toast);
  }

  stamp_message_list_set_conversation (self->message_list, NULL, NULL);

  if (is_mobile_view (self))
    adw_navigation_view_pop_to_tag (self->mobile_nav, "main");
}

static void
on_conversation_trash (GtkWidget             *object,
                       StampConversationItem *item,
                       gpointer               user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  trash (self, item);
}

static void
stamp_mail_view_dispose (GObject *object)
{
  StampMailView *self = STAMP_MAIL_VIEW (object);

  stamp_scheduler_shutdown ();

  g_clear_handle_id (&self->load_folder_handler, g_source_remove);
  g_clear_object (&self->actions);
  g_clear_object (&self->account);
  g_clear_object (&self->trash_toast);

  /* FIXME: Does not work with AdwBreakpointBin */
  /* gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_MAIL_VIEW); */

  G_OBJECT_CLASS (stamp_mail_view_parent_class)->dispose (object);
}

static void
stamp_mail_view_get_property (GObject    *object,
                              guint       property_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  StampMailView *self = STAMP_MAIL_VIEW (object);

  switch ((StampMailViewProps)property_id) {
    case PROP_STACK:
      g_value_set_object (value, self->stack);
      break;
  }
}

static void
stamp_mail_view_set_property (GObject      *object,
                              guint         property_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  StampMailView *self = STAMP_MAIL_VIEW (object);

  switch ((StampMailViewProps)property_id) {
    case PROP_STACK:
      g_set_object (&self->stack, g_value_get_object (value));
      break;
  }
}

static gboolean
is_mobile_view (StampMailView *self)
{
  return g_strcmp0 (adw_multi_layout_view_get_layout_name (self->mail_layout), "mobile") == 0;
}

static
void
on_apply_view (AdwBreakpoint *breakpoint,
               gpointer       user_data)
{
  StampMailView *self;
  gboolean show;

  if (!user_data)
    return;

  self = STAMP_MAIL_VIEW (user_data);
  show = is_mobile_view (self);

  stamp_consersation_list_set_show_buttons (self->conversation_list, show);
  stamp_conversation_list_set_key_capture_widget (self->conversation_list, !show);
  stamp_message_list_set_key_capture_widget (self->message_list, !show);

  if (show && user_data) {
    const gchar *layout = adw_multi_layout_view_get_layout_name (self->mail_layout);

    if (g_strcmp0 (layout, "mobile") == 0) {
      const gchar *tag = adw_navigation_view_get_visible_page_tag (self->mobile_nav);

      if (g_strcmp0 (tag, "main") == 0 && stamp_message_list_has_conversation (self->message_list))
        adw_navigation_view_push_by_tag (self->mobile_nav, "content");
    }
  }
}

void
stamp_mail_view_class_init (StampMailViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/stamp-mail-view.ui");

  object_class->dispose = stamp_mail_view_dispose;
  object_class->set_property = stamp_mail_view_set_property;
  object_class->get_property = stamp_mail_view_get_property;

  obj_properties[PROP_STACK] = g_param_spec_object ("stack", NULL, NULL,
                                                    ADW_TYPE_VIEW_STACK,
                                                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (obj_properties), obj_properties);

  gtk_widget_class_bind_template_child (widget_class, StampMailView, mail_layout);
  gtk_widget_class_bind_template_child (widget_class, StampMailView, folder_list);
  gtk_widget_class_bind_template_child (widget_class, StampMailView, message_list);
  gtk_widget_class_bind_template_child (widget_class, StampMailView, conversation_list);
  gtk_widget_class_bind_template_child (widget_class, StampMailView, toast_overlay);
  gtk_widget_class_bind_template_child (widget_class, StampMailView, desktop_paned);
  gtk_widget_class_bind_template_child (widget_class, StampMailView, tablet_paned);
  gtk_widget_class_bind_template_child (widget_class, StampMailView, tablet_osv);
  gtk_widget_class_bind_template_child (widget_class, StampMailView, mobile_osv);
  gtk_widget_class_bind_template_child (widget_class, StampMailView, mobile_nav);
  gtk_widget_class_bind_template_callback (widget_class, on_details_hidden);
  gtk_widget_class_bind_template_callback (widget_class, on_apply_view);
  gtk_widget_class_bind_template_callback (widget_class, on_folder_selected);
  gtk_widget_class_bind_template_callback (widget_class, on_collection_selected);
  gtk_widget_class_bind_template_callback (widget_class, on_conversation_selected);
  gtk_widget_class_bind_template_callback (widget_class, on_conversation_trash);
}

static void
on_reply (GSimpleAction *action,
          GVariant      *parameter,
          gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  stamp_message_list_compose (self->message_list, STAMP_COMPOSER_REPLY, parameter);
}

static void
on_reply_with_template (GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  stamp_message_list_reply_with_template (self->message_list, g_variant_get_string (parameter, NULL));
}

static void
on_reply_all (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  stamp_message_list_compose (self->message_list, STAMP_COMPOSER_REPLY_ALL, parameter);
}

static void
on_forward (GSimpleAction *action,
            GVariant      *parameter,
            gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  stamp_message_list_compose (self->message_list, STAMP_COMPOSER_FORWARD, parameter);
}

static void
on_trash (GSimpleAction *action,
          GVariant      *parameter,
          gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  trash (self, NULL);
}

static void
on_undo_trash (GSimpleAction *action,
               GVariant      *parameter,
               gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  if (!self->trash_toast)
    return;

  stamp_conversation_list_undo_trash (self->conversation_list);
  adw_toast_dismiss (self->trash_toast);
}

static void
on_composer_new (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  stamp_message_list_compose (self->message_list, STAMP_COMPOSER_NEW, parameter);
}

static void
on_edit (GSimpleAction *action,
         GVariant      *parameter,
         gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  stamp_message_list_compose (self->message_list, STAMP_COMPOSER_DRAFT, parameter);
}

static void
on_use_template (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  stamp_message_list_compose (self->message_list, STAMP_COMPOSER_TEMPLATE, parameter);
}

static void
junk (StampMailView         *self,
      StampConversationItem *item)
{
  AdwToast *toast;

  stamp_conversation_list_junk (self->conversation_list, item);

  toast = adw_toast_new (_("Conversations moved to junk"));
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

static void
on_junk (GSimpleAction *action,
         GVariant      *parameter,
         gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  junk (self, NULL);
}

static void
on_archive (GSimpleAction *action,
            GVariant      *parameter,
            gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);
  AdwToast *toast;

  stamp_conversation_list_archive (self->conversation_list, NULL);

  toast = adw_toast_new (_("Conversations moved to archives"));
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

static void
on_refresh (GSimpleAction *action,
            GVariant      *parameter,
            gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);

  stamp_conversation_list_refresh (self->conversation_list);
}

static void
on_toggle_collapsed (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);
  AdwOverlaySplitView *osv = get_current_osv (self);

  if (!osv)
    return;

  adw_overlay_split_view_set_collapsed (osv, !adw_overlay_split_view_get_collapsed (osv));
}

static const GActionEntry stamp_mail_view_action_entries[] = {
  { .name = "composer-new", .activate = on_composer_new },
  { .name = "edit", .activate = on_edit },
  { .name = "use-template", .activate = on_use_template },
  { .name = "forward", .activate = on_forward, .parameter_type = "s" },
  { .name = "forward-current", .activate = on_forward },
  { .name = "reply", .activate = on_reply, .parameter_type = "s" },
  { .name = "reply-all", .activate = on_reply_all, .parameter_type = "s" },
  { .name = "reply-all-current", .activate = on_reply_all },
  { .name = "reply-current", .activate = on_reply },
  { .name = "reply-with-template", .activate = on_reply_with_template, .parameter_type = "s" },
  { .name = "trash", .activate = on_trash },
  { .name = "undo-trash", .activate = on_undo_trash },
  { .name = "junk", .activate = on_junk },
  { .name = "refresh", .activate = on_refresh },
  { .name = "archive", .activate = on_archive },
  { .name = "toggle-collapsed", .activate = on_toggle_collapsed },
};

static void
on_toggle_sidebar (GtkToggleButton *btn G_GNUC_UNUSED,
                   gpointer             user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);
  AdwOverlaySplitView *osv = get_current_osv (self);

  if (!osv)
    return;

  stamp_folder_list_unselect (self->folder_list);
  adw_overlay_split_view_set_show_sidebar (osv, !adw_overlay_split_view_get_show_sidebar (osv));
}

static void
on_layout_changed (AdwMultiLayoutView *view,
                   GParamSpec *ps      G_GNUC_UNUSED,
                   gpointer            user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);
  const gchar *name = adw_multi_layout_view_get_layout_name (view);
  gboolean narrow = g_strcmp0 (name, "desktop") != 0;
  GtkToggleButton *toggle_button = stamp_conversation_list_get_sidebar_button (self->conversation_list);

  gtk_widget_set_visible (GTK_WIDGET (toggle_button), narrow);

  stamp_message_list_set_mobile_mode (self->message_list, g_strcmp0 (name, "mobile") == 0);

  if (!narrow && self->saved_paned_pos > 50) {
    gtk_paned_set_position (self->desktop_paned, self->saved_paned_pos);
    gtk_paned_set_position (self->tablet_paned, self->saved_paned_pos);
  }
}

static void
on_paned_changed (GtkPaned      *paned,
                  GParamSpec *ps G_GNUC_UNUSED,
                  gpointer       user_data)
{
  StampMailView *self = STAMP_MAIL_VIEW (user_data);
  gint pos = gtk_paned_get_position (paned);

  if (pos < 50)
    return;

  self->saved_paned_pos = pos;
}

typedef struct {
  const gchar *action;
  const gchar *shortcut;
} Shortcut;

static const Shortcut MailShortcuts[] = {
  { "mail.composer-new", "<primary>n" },
  { "mail.forward-current", "<primary><shift>f" },
  { "mail.reply-all-current", "<primary><shift>r" },
  { "mail.reply-current", "<primary>r" },
  { "mail.mark-flag-current", "<primary>s" },
  { "mail.mark-read-current", "<primary><shift>i" },
  { "mail.mark-unflag-current", "<primary><shift>s" },
  { "mail.mark-unread-current", "<primary><shift>u" },
  { "mail.print-current", "<primary>p" },
  { "mail.trash", "<primary>d" },
  { "mail.junk", "<primary>j" },
  { "mail.archive", "<primary>a" },
  { "mail.trash", "Delete" },
  { "mail.undo-trash", "<primary>z" },
  { "mail.view-source", "<primary>s" },
  { "mail.refresh", "F5" },
  { "mail.toggle-collapsed", "F9" },
};

static void
stamp_mail_view_navigate_back (StampMailView *self)
{
  const gchar *layout = adw_multi_layout_view_get_layout_name (self->mail_layout);

  if (g_strcmp0 (layout, "mobile") == 0)
    adw_navigation_view_pop (self->mobile_nav);
}

void
stamp_mail_view_init (StampMailView *self)
{
  GtkEventController *controller;

  gtk_widget_init_template (GTK_WIDGET (self));

  adw_multi_layout_view_set_layout_name (self->mail_layout, "desktop");

  stamp_conversation_list_set_key_capture_widget (self->conversation_list, TRUE);
  stamp_message_list_set_key_capture_widget (self->message_list, TRUE);

  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions),
                                   stamp_mail_view_action_entries,
                                   G_N_ELEMENTS (stamp_mail_view_action_entries),
                                   self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "mail", G_ACTION_GROUP (self->actions));

  /* Note: Blueprint currently (January 2026) does not handled GtkNamedActions, so add it in code */
  controller = gtk_shortcut_controller_new ();
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  for (guint idx = 0; idx < G_N_ELEMENTS (MailShortcuts); idx++) {
    GtkShortcut *shortcut;

    shortcut = gtk_shortcut_new (gtk_shortcut_trigger_parse_string (MailShortcuts[idx].shortcut), gtk_named_action_new (MailShortcuts[idx].action));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (controller), shortcut);
  }

  g_signal_connect (self->mail_layout, "notify::layout-name", G_CALLBACK (on_layout_changed), self);

  self->saved_paned_pos = 360;

  g_signal_connect (self->mail_layout, "notify::layout-name", G_CALLBACK (on_layout_changed), self);
  g_signal_connect (stamp_conversation_list_get_sidebar_button (self->conversation_list), "clicked", G_CALLBACK (on_toggle_sidebar), self);

  stamp_folder_list_set_conversation_list (self->folder_list, self->conversation_list);

  /* Remember paned position */
  g_signal_connect (self->desktop_paned, "notify::position", G_CALLBACK (on_paned_changed), self);
  g_signal_connect (self->tablet_paned, "notify::position", G_CALLBACK (on_paned_changed), self);

  /* Synchronize button state with show-sidebar */
  g_signal_connect_swapped (self->tablet_osv, "notify::show-sidebar", G_CALLBACK (on_sidebar_visibility_changed), self);
  g_signal_connect_swapped (self->mobile_osv, "notify::show-sidebar", G_CALLBACK (on_sidebar_visibility_changed), self);

  g_signal_connect_swapped (self->message_list, "switch-conversation",
                            G_CALLBACK (stamp_conversation_list_select_relative), self->conversation_list);
  g_signal_connect_swapped (self->message_list, "navigate-back",
                            G_CALLBACK (stamp_mail_view_navigate_back), self);

  stamp_scheduler_init ();
}

GtkWidget *
stamp_mail_view_new (void)
{
  return g_object_new (STAMP_TYPE_MAIL_VIEW, NULL);
}

void
stamp_mail_view_search_contact (StampMailView *self,
                                const gchar   *mail)
{
  stamp_mail_conversation_list_search_contact (self->conversation_list, mail);

  if (is_mobile_view (self))
    adw_navigation_view_pop_to_tag (self->mobile_nav, "main");
}

GSimpleActionGroup *
stamp_mail_view_get_action_group (StampMailView *self)
{
  return self->actions;
}

void
stamp_mail_view_show_toast (StampMailView *self,
                            const gchar   *message)
{
  AdwToast *toast;

  if (!self)
    return;

  toast = adw_toast_new (message);
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

StampConversationList *
stamp_mail_view_get_conversation_list (StampMailView *self)
{
  return self->conversation_list;
}
