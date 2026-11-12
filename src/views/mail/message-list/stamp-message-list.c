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

#include "stamp-message-list.h"

#include <glib/gi18n.h>

#include "stamp-conversation-item.h"
#include "stamp-conversation-list.h"
#include "stamp-input-helpers.h"
#include "stamp-mail-view.h"
#include "stamp-message-header.h"
#include "stamp-message-list-item.h"
#include "stamp-mime-parser.h"
#include "stamp-session.h"
#include "stamp-window.h"

struct _StampMessageList {
  AdwBreakpointBin parent_instance;

  GtkWidget *scrolled_window;
  GtkWidget *list_box;
  GtkWidget *message_title;
  GtkWidget *hover_url;
  GtkWidget *stack;
  GtkWidget *search_bar;
  GtkWidget *search_entry;
  GtkWidget *edit_button;
  GtkWidget *use_template_button;
  GtkWidget *external;

  GtkWidget *reply_btn;
  GtkWidget *reply_all_btn;
  GtkWidget *forward_btn;
  GtkWidget *delete_btn;
  GtkWidget *spam_btn;
  GtkWidget *archive_btn;
  GtkWidget *toolbar;

  GHashTable *messages;
  StampAccount *account;

  gchar *subject;

  GtkWidget *carousel;
  GtkWidget *prev_page;
  GtkWidget *next_page;
  GtkWidget *message_stack;
  gboolean rebuilding;
  guint scroll_to_bottom_handler;
  guint adjacent_pages_handler;
  guint navigate_back_handler;

  guint pending_searches;
  gboolean search_found_match;

  gboolean mobile_mode;
};

G_DEFINE_FINAL_TYPE (StampMessageList, stamp_message_list, ADW_TYPE_BREAKPOINT_BIN);

enum {
  HOVERING_OVER_LINK,
  LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = { 0 };

static void
check_search_results (StampMessageList *self)
{
  gtk_widget_remove_css_class (self->search_entry, "error");

  if (!self->search_found_match)
    gtk_widget_add_css_class (self->search_entry, "error");
}

static void
on_item_search_done (StampMessageListItem *item,
                     gboolean              found,
                     gpointer              user_data)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (user_data);

  self->pending_searches--;
  if (found)
    self->search_found_match = TRUE;

  if (self->pending_searches == 0)
    check_search_results (self);
}

static void
on_message_search_entry_changed (GtkWidget *search_entry,
                                 gpointer   user_data)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (user_data);
  GList *keys;
  const gchar *search_text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));

  if (!self->messages)
    return;

  if (strlen (search_text) == 0) {
    gtk_widget_remove_css_class (self->search_entry, "error");
    return;
  }

  self->pending_searches = g_hash_table_size (self->messages);
  self->search_found_match = FALSE;

  keys = g_hash_table_get_keys (self->messages);
  for (GList *iter = keys; iter && iter->data; iter = g_list_next (iter)) {
    StampMessageListItem *item = STAMP_MESSAGE_LIST_ITEM (g_hash_table_lookup (self->messages, iter->data));

    stamp_message_list_item_search (item, search_text, on_item_search_done, self);
  }

  g_list_free (keys);
}

static void
stamp_message_list_dispose (GObject *object)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (object);

  g_clear_pointer (&self->messages, g_hash_table_unref);
  g_clear_handle_id (&self->scroll_to_bottom_handler, g_source_remove);
  g_clear_handle_id (&self->adjacent_pages_handler, g_source_remove);
  g_clear_handle_id (&self->navigate_back_handler, g_source_remove);
  g_clear_object (&self->prev_page);
  g_clear_object (&self->next_page);

  G_OBJECT_CLASS (stamp_message_list_parent_class)->dispose (object);
}

enum {
  SWITCH_CONVERSATION,
  NAVIGATE_BACK,
};

static gint switch_signals[NAVIGATE_BACK + 1] = { 0 };

static void
on_carousel_page_changed (AdwCarousel *carousel,
                          guint        index,
                          gpointer     user_data)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (user_data);
  GtkWidget *page;

  if (self->rebuilding)
    return;

  page = adw_carousel_get_nth_page (carousel, index);

  if (page == self->prev_page)
    g_signal_emit (self, switch_signals[SWITCH_CONVERSATION], 0, -1);
  else if (page == self->next_page)
    g_signal_emit (self, switch_signals[SWITCH_CONVERSATION], 0, 1);
}

static void
emit_navigate_back_idle (gpointer user_data)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (user_data);

  g_signal_emit (self, switch_signals[NAVIGATE_BACK], 0);
  self->navigate_back_handler = 0;
}

static void
on_drag_begin (GtkGestureDrag *gesture,
               gdouble         start_x,
               gdouble         start_y,
               gpointer        user_data)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (user_data);
  GtkWidget *bin = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  GtkWidget *picked = gtk_widget_pick (bin, start_x, start_y, GTK_PICK_DEFAULT);
  gboolean at_left_edge = start_x < 50;
  gboolean on_header = FALSE;

  for (GtkWidget *w = picked; w && w != bin; w = gtk_widget_get_parent (w)) {
    if (STAMP_IS_MESSAGE_HEADER (w)) {
      on_header = TRUE;
      break;
    }
  }

  adw_carousel_set_interactive (ADW_CAROUSEL (self->carousel), on_header && !at_left_edge);

  g_object_set_data (G_OBJECT (gesture), "triggered", GINT_TO_POINTER (FALSE));
  g_object_set_data (G_OBJECT (gesture), "back-swipe", GINT_TO_POINTER (!on_header || at_left_edge));
}

static void
on_drag_update (GtkGestureDrag *gesture,
                gdouble         offset_x,
                gdouble         offset_y,
                gpointer        user_data)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (user_data);

  if (g_object_get_data (G_OBJECT (gesture), "triggered"))
    return;

  if (g_object_get_data (G_OBJECT (gesture), "back-swipe") && offset_x > 20 && offset_x > 2 * ABS (offset_y)) {
    g_object_set_data (G_OBJECT (gesture), "triggered", GINT_TO_POINTER (TRUE));
    g_clear_handle_id (&self->navigate_back_handler, g_source_remove);
    self->navigate_back_handler = g_idle_add_once (emit_navigate_back_idle, self);
  }
}

static void
update_mobile_toolbar (StampMessageList *self,
                       gint              width)
{
  gboolean narrow = width < 500;

  gtk_widget_set_visible (self->reply_btn, !narrow);
  gtk_widget_set_visible (self->reply_all_btn, !narrow);
  gtk_widget_set_visible (self->forward_btn, !narrow);
  gtk_widget_set_visible (self->delete_btn, !narrow);
  gtk_widget_set_visible (self->spam_btn, !narrow);
  gtk_widget_set_visible (self->archive_btn, !narrow);
  gtk_widget_set_visible (self->toolbar, narrow);
}

static void
stamp_message_list_size_allocate (GtkWidget *widget,
                                  gint       width,
                                  gint       height,
                                  gint       baseline)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (widget);

  GTK_WIDGET_CLASS (stamp_message_list_parent_class)->size_allocate (widget, width, height, baseline);

  if (self->mobile_mode)
    update_mobile_toolbar (self, width);
}

gboolean
stamp_message_list_get_mobile_mode (StampMessageList *self)
{
  return self->mobile_mode;
}

void
stamp_message_list_set_mobile_mode (StampMessageList *self,
                                    gboolean          mobile)
{
  self->mobile_mode = mobile;

  for (GtkWidget *row = gtk_widget_get_first_child (self->list_box); row; row = gtk_widget_get_next_sibling (row)) {
    if (STAMP_IS_MESSAGE_LIST_ITEM (row))
      stamp_message_list_item_set_mobile_mode (STAMP_MESSAGE_LIST_ITEM (row), mobile);
  }

  if (mobile) {
    update_mobile_toolbar (self, gtk_widget_get_width (GTK_WIDGET (self)));
  } else {
    gtk_widget_set_visible (self->reply_btn, TRUE);
    gtk_widget_set_visible (self->reply_all_btn, TRUE);
    gtk_widget_set_visible (self->forward_btn, TRUE);
    gtk_widget_set_visible (self->delete_btn, TRUE);
    gtk_widget_set_visible (self->spam_btn, TRUE);
    gtk_widget_set_visible (self->archive_btn, TRUE);
    gtk_widget_set_visible (self->toolbar, FALSE);
  }
}

static void
stamp_message_list_class_init (StampMessageListClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->dispose = stamp_message_list_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/message-list/stamp-message-list.ui");

  gtk_widget_class_bind_template_child (widget_class, StampMessageList, scrolled_window);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, list_box);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, message_title);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, hover_url);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, stack);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, search_bar);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, search_entry);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, edit_button);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, use_template_button);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, external);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, reply_btn);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, reply_all_btn);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, forward_btn);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, delete_btn);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, spam_btn);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, archive_btn);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, toolbar);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, carousel);
  gtk_widget_class_bind_template_child (widget_class, StampMessageList, message_stack);

  gtk_widget_class_bind_template_callback (widget_class, on_message_search_entry_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_drag_begin);
  gtk_widget_class_bind_template_callback (widget_class, on_drag_update);
  gtk_widget_class_bind_template_callback (widget_class, on_carousel_page_changed);

  widget_class->size_allocate = stamp_message_list_size_allocate;

  signals[HOVERING_OVER_LINK] = g_signal_new ("hovering-over-link", G_OBJECT_CLASS_TYPE (klass),
                                              G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                              0, NULL, NULL, NULL,
                                              G_TYPE_NONE,
                                              2, G_TYPE_STRING, G_TYPE_STRING);

  switch_signals[SWITCH_CONVERSATION] = g_signal_new ("switch-conversation", G_OBJECT_CLASS_TYPE (klass),
                                                      G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                                      0, NULL, NULL, NULL,
                                                      G_TYPE_NONE,
                                                      1, G_TYPE_INT);

  switch_signals[NAVIGATE_BACK] = g_signal_new ("navigate-back", G_OBJECT_CLASS_TYPE (klass),
                                                G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                                0, NULL, NULL, NULL,
                                                G_TYPE_NONE, 0);
}

static gint
sort_mails (GtkListBoxRow *row1,
            GtkListBoxRow *row2,
            gpointer       user_data)
{
  StampMessageListItem *item1 = STAMP_MESSAGE_LIST_ITEM (row1);
  StampMessageListItem *item2 = STAMP_MESSAGE_LIST_ITEM (row2);
  guint ts1 = stamp_message_list_item_get_timestamp (item1);
  guint ts2 = stamp_message_list_item_get_timestamp (item2);

  if (ts1 < ts2)
    return -1;
  if (ts1 > ts2)
    return 1;
  return 0;
}

static gboolean
subject_changed (const gchar *subject,
                 gchar       *new_subject)
{
  /* Skip prefix Re: / Fwd: / Yes: / No: / .... */
  g_auto (GStrv) split = g_strsplit (new_subject, ":", 2);
  gchar *check = new_subject;
  gboolean ret;

  if (g_strv_length (split) == 2)
    check = split[1];

  g_strstrip (check);
  ret = g_strstr_len (subject, -1, check) == NULL;

  return ret;
}

static void
update_header (GtkListBoxRow *row,
               GtkListBoxRow *before,
               gpointer       user_data)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (user_data);
  StampMessageListItem *item;
  const gchar *subject;

  if (!before)
    return;

  item = STAMP_MESSAGE_LIST_ITEM (row);
  subject = camel_message_info_get_subject (stamp_message_list_item_get_message_info (item));
  if (subject_changed (self->subject, (char *)subject)) {
    GtkWidget *label = gtk_label_new (subject);

    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_label_set_wrap (GTK_LABEL (label), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable (GTK_LABEL (label), TRUE);
    gtk_widget_add_css_class (label, "title-2");
    gtk_widget_add_css_class (label, "message-list-header");
    gtk_widget_add_css_class (label, "message-list-header-between");

    g_set_str (&self->subject, subject);

    gtk_list_box_row_set_header (row, label);
  } else {
    gtk_list_box_row_set_header (row, NULL);
  }
}

static void
mark_visible_messages_as_read (StampMessageList *self)
{
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));
  GtkWidget *row;
  gdouble viewport_height = gtk_adjustment_get_page_size (adj);

  for (row = gtk_widget_get_first_child (GTK_WIDGET (self->list_box)); row != NULL; row = gtk_widget_get_next_sibling (row)) {
    StampMessageListItem *item;
    const CamelMessageInfo *const_info;
    graphene_point_t in_point;
    graphene_point_t out_point;
    gdouble row_height;

    if (!STAMP_IS_MESSAGE_LIST_ITEM (row))
      continue;

    item = STAMP_MESSAGE_LIST_ITEM (row);
    const_info = stamp_message_list_item_get_message_info (item);
    if ((camel_message_info_get_flags (const_info) & CAMEL_MESSAGE_SEEN) != 0)
      continue;

    graphene_point_init (&in_point, 0, 0);

    if (!gtk_widget_compute_point (row, GTK_WIDGET (self->scrolled_window), &in_point, &out_point))
      continue;

    row_height = gtk_widget_get_height (row);

    if (out_point.y + row_height > 0 && out_point.y < viewport_height) {
      CamelMessageInfo *info;

      info = (CamelMessageInfo *)const_info;
      camel_message_info_set_flags (info, CAMEL_MESSAGE_SEEN, ~0);
    }
  }
}

static void
on_vadjustment_value_changed (GtkAdjustment *adj,
                              gpointer       user_data)
{
  mark_visible_messages_as_read (STAMP_MESSAGE_LIST (user_data));
}

static void
stamp_message_list_init (StampMessageList *self)
{
  GtkAdjustment *adj;

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_stack_set_visible_child_name (GTK_STACK (self->message_stack), "no-conversation");

  stamp_message_list_hovering_over_link (self, NULL, NULL);

  gtk_list_box_set_sort_func (GTK_LIST_BOX (self->list_box), sort_mails, NULL, NULL);
  gtk_list_box_set_header_func (GTK_LIST_BOX (self->list_box), update_header, self, NULL);

  gtk_search_bar_connect_entry (GTK_SEARCH_BAR (self->search_bar), GTK_EDITABLE (self->search_entry));

  adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_vadjustment_value_changed), self);
}

GtkWidget *
stamp_message_list_new (void)
{
  return g_object_new (STAMP_TYPE_MESSAGE_LIST, NULL);
}

static void
go_down (StampMessageList      *self,
         CamelFolderThreadNode *node)
{
  CamelFolderThreadNode *current_node = node;

  while (current_node) {
    GtkWidget *item = stamp_message_list_item_new (self->account, current_node);
    CamelMessageInfo *message = camel_folder_thread_node_get_item (current_node);

    gtk_list_box_append (GTK_LIST_BOX (self->list_box), item);
    if ((camel_message_info_get_flags (message) & CAMEL_MESSAGE_SEEN) == 0) {
      stamp_message_list_item_set_expanded (STAMP_MESSAGE_LIST_ITEM (item), TRUE);
    }
    g_hash_table_insert (self->messages, g_strdup (camel_message_info_get_uid (message)), g_object_ref (item));

    if (camel_folder_thread_node_get_next (current_node))
      go_down (self, camel_folder_thread_node_get_next (current_node));

    current_node = camel_folder_thread_node_get_child (current_node);
  }
}

static void
scroll_to_bottom (gpointer user_data)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (user_data);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));
  GtkWidget *first_unread = NULL;
  GtkWidget *row;
  gboolean found_unread = FALSE;

  for (row = gtk_widget_get_first_child (GTK_WIDGET (self->list_box)); row != NULL; row = gtk_widget_get_next_sibling (row)) {
    const CamelMessageInfo *info;

    if (!STAMP_IS_MESSAGE_LIST_ITEM (row))
      continue;

    info = stamp_message_list_item_get_message_info (STAMP_MESSAGE_LIST_ITEM (row));

    if ((camel_message_info_get_flags (info) & CAMEL_MESSAGE_SEEN) == 0) {
      found_unread = TRUE;
      first_unread = row;
      break;
    }
  }

  if (found_unread && first_unread) {
    graphene_point_t in_point;
    graphene_point_t out_point;

    graphene_point_init (&in_point, 0, 0);
    if (gtk_widget_compute_point (first_unread, GTK_WIDGET (self->list_box), &in_point, &out_point))
      gtk_adjustment_set_value (adj, out_point.y);
  }

  self->scroll_to_bottom_handler = 0;

  mark_visible_messages_as_read (self);
}

static GtkWidget *
find_last_of_type (GtkListBox *listbox,
                   GType       type)
{
  GtkWidget *result = NULL;

  for (GtkWidget *row = gtk_widget_get_first_child (GTK_WIDGET (listbox)); row != NULL; row = gtk_widget_get_next_sibling (row)) {
    if (G_TYPE_CHECK_INSTANCE_TYPE (row, type))
      result = row;
  }

  return result;
}

static void
populate_list_box_from_thread (StampAccount          *account,
                               GtkListBox            *list_box,
                               CamelFolderThreadNode *node)
{
  CamelFolderThreadNode *current_node = node;

  while (current_node) {
    GtkWidget *item = stamp_message_list_item_new (account, current_node);
    CamelMessageInfo *message = camel_folder_thread_node_get_item (current_node);

    gtk_list_box_append (list_box, item);
    if ((camel_message_info_get_flags (message) & CAMEL_MESSAGE_SEEN) == 0) {
      stamp_message_list_item_set_expanded (STAMP_MESSAGE_LIST_ITEM (item), TRUE);
    }

    if (camel_folder_thread_node_get_next (current_node))
      populate_list_box_from_thread (account, list_box, camel_folder_thread_node_get_next (current_node));

    current_node = camel_folder_thread_node_get_child (current_node);
  }
}

static GtkWidget *
create_carousel_page_view (StampAccount          *account,
                           CamelFolderThreadNode *node)
{
  GtkWidget *toolbar_view;
  GtkWidget *header_bar;
  GtkWidget *scrolled;
  GtkWidget *viewport;
  GtkWidget *list_box;
  GtkWidget *subject_label;
  GtkWidget *content_box;
  GtkWidget *header_box;
  GtkWidget *item;
  CamelMessageInfo *root_msg;
  CamelMessageInfo *message;

  toolbar_view = adw_toolbar_view_new ();
  gtk_widget_set_hexpand (toolbar_view, TRUE);
  gtk_widget_set_vexpand (toolbar_view, TRUE);

  header_bar = adw_header_bar_new ();
  adw_header_bar_set_show_title (ADW_HEADER_BAR (header_bar), FALSE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header_bar);

  message = camel_folder_thread_node_get_item (node);

  subject_label = gtk_label_new (camel_message_info_get_subject (message));
  gtk_label_set_xalign (GTK_LABEL (subject_label), 0);
  gtk_label_set_wrap (GTK_LABEL (subject_label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (subject_label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (subject_label, "title-2");

  content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (content_box, TRUE);
  gtk_widget_set_vexpand (content_box, TRUE);
  gtk_widget_add_css_class (content_box, "message-list-conversation");

  header_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (header_box), subject_label);
  gtk_box_append (GTK_BOX (content_box), header_box);

  list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list_box), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list_box, "message-list");
  gtk_widget_add_css_class (list_box, "background");

  viewport = gtk_viewport_new (NULL, NULL);
  gtk_viewport_set_scroll_to_focus (GTK_VIEWPORT (viewport), FALSE);
  gtk_viewport_set_child (GTK_VIEWPORT (viewport), list_box);

  scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand (scrolled, TRUE);
  gtk_widget_set_vexpand (scrolled, TRUE);
  gtk_widget_set_focusable (scrolled, FALSE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), viewport);

  root_msg = camel_folder_thread_node_get_item (node);
  item = stamp_message_list_item_new (account, node);
  gtk_list_box_append (GTK_LIST_BOX (list_box), item);
  if ((camel_message_info_get_flags (root_msg) & CAMEL_MESSAGE_SEEN) == 0)
    stamp_message_list_item_set_expanded (STAMP_MESSAGE_LIST_ITEM (item), TRUE);

  if (camel_folder_thread_node_get_child (node))
    populate_list_box_from_thread (account, GTK_LIST_BOX (list_box), camel_folder_thread_node_get_child (node));

  gtk_box_append (GTK_BOX (content_box), scrolled);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), content_box);

  return toolbar_view;
}

static gboolean
populate_adjacent_pages (gpointer user_data)
{
  StampMessageList *self = STAMP_MESSAGE_LIST (user_data);
  StampWindow *window = STAMP_WINDOW (stamp_get_main_window ());
  StampConversationList *conv_list = stamp_mail_view_get_conversation_list (stamp_window_get_mail_view (window));
  StampConversationItem *adjacent;
  GtkWidget *prev_child;
  GtkWidget *next_child;

  self->adjacent_pages_handler = 0;

  self->rebuilding = TRUE;

  adjacent = stamp_conversation_list_get_adjacent_item (conv_list, -1);
  if (!adjacent && gtk_widget_get_parent (self->prev_page))
    adw_carousel_remove (ADW_CAROUSEL (self->carousel), self->prev_page);
  if (adjacent) {
    if (!gtk_widget_get_parent (self->prev_page))
      adw_carousel_prepend (ADW_CAROUSEL (self->carousel), self->prev_page);

    prev_child = gtk_widget_get_first_child (self->prev_page);

    while (prev_child) {
      GtkWidget *next = gtk_widget_get_next_sibling (prev_child);
      gtk_box_remove (GTK_BOX (self->prev_page), prev_child);
      prev_child = next;
    }

    gtk_box_append (GTK_BOX (self->prev_page), create_carousel_page_view (self->account, stamp_conversation_item_get_node (adjacent)));
    g_object_unref (adjacent);
  }

  adjacent = stamp_conversation_list_get_adjacent_item (conv_list, 1);
  if (!adjacent && gtk_widget_get_parent (self->next_page))
    adw_carousel_remove (ADW_CAROUSEL (self->carousel), self->next_page);
  if (adjacent) {
    if (!gtk_widget_get_parent (self->next_page))
      adw_carousel_append (ADW_CAROUSEL (self->carousel), self->next_page);

    next_child = gtk_widget_get_first_child (self->next_page);

    while (next_child) {
      GtkWidget *next = gtk_widget_get_next_sibling (next_child);
      gtk_box_remove (GTK_BOX (self->next_page), next_child);
      next_child = next;
    }

    gtk_box_append (GTK_BOX (self->next_page), create_carousel_page_view (self->account, stamp_conversation_item_get_node (adjacent)));
    g_object_unref (adjacent);
  }

  adw_carousel_scroll_to (ADW_CAROUSEL (self->carousel), self->message_stack, FALSE);
  self->rebuilding = FALSE;

  return G_SOURCE_REMOVE;
}

void
stamp_message_list_set_conversation (StampMessageList      *self,
                                     StampAccount          *account,
                                     CamelFolderThreadNode *node)
{
  GtkWidget *item;
  GtkWidget *child;
  CamelMessageInfo *message;
  GAction *action;
  GSimpleActionGroup *action_group;
  StampWindow *window = STAMP_WINDOW (stamp_get_main_window ());
  StampMailView *mail_view = stamp_window_get_mail_view (window);
  CamelFolderSummary *summary;
  CamelFolder *folder;
  const gchar *fname;
  gboolean draft_folder = FALSE;
  gboolean templates_folder;

  self->account = account;

  gtk_widget_set_visible (self->archive_btn, account && stamp_account_get_mail_archives_folder (account) != NULL);

  self->rebuilding = TRUE;

  if (!self->prev_page) {
    self->prev_page = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_set_hexpand (self->prev_page, TRUE);
    gtk_widget_set_vexpand (self->prev_page, TRUE);
  }
  if (!self->next_page) {
    self->next_page = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_set_hexpand (self->next_page, TRUE);
    gtk_widget_set_vexpand (self->next_page, TRUE);
  }

  if (gtk_widget_get_parent (self->prev_page))
    adw_carousel_remove (ADW_CAROUSEL (self->carousel), self->prev_page);
  if (gtk_widget_get_parent (self->next_page))
    adw_carousel_remove (ADW_CAROUSEL (self->carousel), self->next_page);

  g_clear_handle_id (&self->adjacent_pages_handler, g_source_remove);
  self->adjacent_pages_handler = g_idle_add_full (G_PRIORITY_LOW, populate_adjacent_pages, self, NULL);

  adw_carousel_scroll_to (ADW_CAROUSEL (self->carousel), self->message_stack, FALSE);
  self->rebuilding = FALSE;

  gtk_list_box_remove_all (GTK_LIST_BOX (self->list_box));

  g_clear_pointer (&self->messages, g_hash_table_unref);
  action_group = stamp_mail_view_get_action_group (mail_view);

  action = g_action_map_lookup_action (G_ACTION_MAP (action_group), "reply-current");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), node != NULL);

  action = g_action_map_lookup_action (G_ACTION_MAP (action_group), "reply-all-current");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), node != NULL);

  action = g_action_map_lookup_action (G_ACTION_MAP (action_group), "reply-with-template");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), node != NULL);

  action = g_action_map_lookup_action (G_ACTION_MAP (action_group), "forward-current");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), node != NULL);

  if (!node) {
    /* Show empty stack */
    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
    gtk_stack_set_visible_child_name (GTK_STACK (self->message_stack), "no-conversation");
    return;
  }

  gtk_stack_set_visible_child_name (GTK_STACK (self->message_stack), "content");

  /* E-D-S does not mark messages as DRAFT nor the folder as TYPE_DRAFTS for MS365…, check full name for the moment
   */
  message = camel_folder_thread_node_get_item (node);
  g_object_get (G_OBJECT (message), "summary", &summary, NULL);
  folder = camel_folder_summary_get_folder (summary);
  fname = camel_folder_get_full_name (folder);
  action = g_action_map_lookup_action (G_ACTION_MAP (action_group), "edit");

  if (g_strrstr (fname, "Drafts") || g_strrstr (fname, "Entwürfe")) {
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), TRUE);
    draft_folder = TRUE;
  } else {
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), FALSE);
  }

  gtk_widget_set_visible (self->edit_button, draft_folder);

  templates_folder = folder == stamp_account_get_mail_templates_folder (self->account);
  action = g_action_map_lookup_action (G_ACTION_MAP (action_group), "use-template");
  g_simple_action_set_enabled (G_SIMPLE_ACTION (action), templates_folder);
  gtk_widget_set_visible (self->use_template_button, templates_folder);

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "content");

  self->messages = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  gtk_label_set_text (GTK_LABEL (self->message_title), camel_message_info_get_subject (camel_folder_thread_node_get_item (node)));

  self->subject = g_strdup (camel_message_info_get_subject (camel_folder_thread_node_get_item (node)));

  item = stamp_message_list_item_new (self->account, node);
  if ((camel_message_info_get_flags (message) & CAMEL_MESSAGE_SEEN) == 0) {
    stamp_message_list_item_set_expanded (STAMP_MESSAGE_LIST_ITEM (item), TRUE);
  }
  gtk_list_box_append (GTK_LIST_BOX (self->list_box), GTK_WIDGET (item));

  g_hash_table_insert (self->messages, g_strdup (camel_message_info_get_uid (message)), g_object_ref (item));
  if (camel_folder_thread_node_get_child (node))
    go_down (self, camel_folder_thread_node_get_child (node));

  child = find_last_of_type (GTK_LIST_BOX (self->list_box), STAMP_TYPE_MESSAGE_LIST_ITEM);
  if (child && STAMP_IS_MESSAGE_LIST_ITEM (child) && STAMP_MESSAGE_LIST_ITEM (child)) {
    StampMessageListItem *list_item = STAMP_MESSAGE_LIST_ITEM (child);

    stamp_message_list_item_set_expanded (list_item, TRUE);
  }

  g_clear_handle_id (&self->scroll_to_bottom_handler, g_source_remove);
  self->scroll_to_bottom_handler = g_idle_add_once (scroll_to_bottom, self);
}

void
stamp_message_list_hovering_over_link (StampMessageList *self,
                                       const gchar      *title,
                                       const gchar      *url)
{
  if (!url) {
    gtk_widget_set_visible (self->hover_url, FALSE);
  } else {
    gtk_label_set_text (GTK_LABEL (self->hover_url), url);
    gtk_widget_set_visible (self->hover_url, TRUE);
  }
}

typedef struct {
  StampMessageList *self;
  StampComposerType type;
  StampMessageListItem *item;
} ComposerData;

static void
on_message_body (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  GtkWidget *composer;
  ComposerData *data = user_data;
  g_autoptr (GError) error = NULL;
  gchar *body;

  body = stamp_message_list_item_get_message_body_html_finish (data->item, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Could not get message body html: %s", error->message);
    g_clear_object (&data->self);
    g_clear_object (&data->item);
    g_clear_pointer (&data, g_free);
    return;
  }

  if (!body) {
    g_warning ("Could not get message body html: empty response");
    g_clear_object (&data->self);
    g_clear_object (&data->item);
    g_clear_pointer (&data, g_free);
    return;
  }

  composer = stamp_composer_new_with_quote (data->type,
                                            stamp_message_list_item_get_uid (data->item),
                                            data->self->account,
                                            stamp_message_list_item_get_web_view (data->item),
                                            stamp_message_list_item_get_message_info (data->item),
                                            stamp_message_list_item_get_message (data->item),
                                            body,
                                            NULL);
  gtk_window_present (GTK_WINDOW (composer));

  g_clear_object (&data->self);
  g_clear_object (&data->item);
  g_clear_pointer (&data, g_free);
}

static GtkWidget *
find_message_item (StampMessageList *self,
                   const gchar      *uid)
{
  GtkWidget *child;

  if (uid && strlen (uid)) {
    child = g_hash_table_lookup (self->messages, uid);
  } else {
    child = gtk_widget_get_last_child (self->list_box);
  }

  /* Ensure that last item is actually a message list item and not a header */
  while (child && !STAMP_IS_MESSAGE_LIST_ITEM (child))
    child = gtk_widget_get_prev_sibling (child);

  return child;
}

void
stamp_message_list_compose (StampMessageList  *self,
                            StampComposerType  type,
                            GVariant          *parameter)
{
  GtkWidget *child;
  ComposerData *data;
  const gchar *uid = NULL;

  if (type == STAMP_COMPOSER_NEW) {
    GtkWidget *composer;

    composer = stamp_composer_new (self->account);
    gtk_window_present (GTK_WINDOW (composer));
    return;
  }

  if (parameter)
    uid = g_variant_get_string (parameter, NULL);

  child = find_message_item (self, uid);
  if (!child)
    return;

  data = g_new0 (ComposerData, 1);

  data->item = g_object_ref (STAMP_MESSAGE_LIST_ITEM (child));
  data->self = g_object_ref (self);
  data->type = type;

  stamp_message_list_item_get_message_body_html (data->item, NULL, on_message_body, data);
}

void
stamp_message_list_reply_with_template (StampMessageList *self,
                                        const gchar      *template_uid)
{
  GtkWidget *child = find_message_item (self, NULL);

  if (!child)
    return;

  stamp_message_list_item_reply_with_template (STAMP_MESSAGE_LIST_ITEM (child), template_uid);
}

void
stamp_message_list_print (StampMessageList *self,
                          GVariant         *parameter)
{
  const gchar *uid = NULL;
  GtkWidget *child;
  StampMessageListItem *item;

  if (parameter)
    uid = g_variant_get_string (parameter, NULL);

  if (uid && strlen (uid)) {
    child = g_hash_table_lookup (self->messages, uid);
  } else {
    child = gtk_widget_get_last_child (self->list_box);
  }

  if (!child)
    return;

  item = STAMP_MESSAGE_LIST_ITEM (child);

  stamp_message_list_item_print (item);
}

void
stamp_message_list_view_source (StampMessageList *self,
                                GVariant         *parameter)
{
  GtkWidget *child;
  StampMessageListItem *item;

  child = gtk_widget_get_last_child (self->list_box);

  if (!child)
    return;

  item = STAMP_MESSAGE_LIST_ITEM (child);

  stamp_message_list_item_view_source (item);
}

void
stamp_message_list_set_external (StampMessageList *self,
                                 gboolean          is_external)
{
  gtk_widget_set_visible (self->external, is_external);
}

void
stamp_message_list_set_key_capture_widget (StampMessageList *self,
                                           gboolean          enable)
{
  gtk_search_bar_set_key_capture_widget (GTK_SEARCH_BAR (self->search_bar),
                                         enable && stamp_input_has_keyboard (self->search_bar) ? self->message_stack : NULL);
}

gboolean
stamp_message_list_has_conversation (StampMessageList *self)
{
  return g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (self->stack)), "empty") != 0;
}
