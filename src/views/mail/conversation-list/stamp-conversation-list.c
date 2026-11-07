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

#include "stamp-conversation-list.h"

#include <camel/camel.h>
#include <glib/gi18n.h>

#include "stamp-account.h"
#include "stamp-collection-item.h"
#include "stamp-composer.h"
#include "stamp-conversation-item.h"
#include "stamp-conversation-list-store.h"
#include "stamp-conversation-row.h"
#include "stamp-item.h"
#include "stamp-mail-view.h"
#include "stamp-message-list.h"
#include "stamp-session.h"
#include "stamp-settings.h"


enum FilterMode {
  FILTER_MODE_ALL,
  FILTER_MODE_STARRED,
  FILTER_MODE_ATTACHMENT,
  FILTER_MODE_UNREAD,
};

enum SortMode {
  SORT_MODE_NEWEST_FIRST,
  SORT_MODE_OLDEST_FIRST,
};

#define THREAD_CACHE_MAX 5

static void
trash_entry_free (gpointer data);

struct _StampConversationList {
  AdwBin parent_instance;

  GtkWidget *listview;
  GtkWidget *conversation_view;
  GtkWidget *search_bar;
  GtkWidget *search_entry;
  GtkWidget *window_title;
  GtkWidget *scroll_to_top;
  GtkWidget *scrolled_window;
  GtkWidget *header_stack;
  GtkWidget *normal_headerbar;
  GtkWidget *selection_headerbar;
  GtkWidget *selection_label;
  GtkWidget *spinner;
  GtkAdjustment *vadj;
  gboolean keep_scroll;
  GtkWidget *sort_or_filter_button;
  GtkWidget *mail_list_stack;
  GtkBitset *selected;
  GtkToggleButton *sidebar_button;
  GtkBox *sort_is_active;
  GtkWidget *new_message;
  GtkWidget *selection_bottom_bar;
  GtkMenuButton *move_selection_button;
  GtkButton *trash_selection_button;
  GtkButton *mark_read_selection_button;

  GListStore *list_store;
  GtkSingleSelection *single_selection;
  GtkNoSelection *multi_selection;
  GCancellable *cancellable;
  GCancellable *thread_cancellable;
  GCancellable *transfer_cancellable;
  GHashTable *folders;
  GList *moved_messages;
  gboolean is_pulling;
  GtkSorter *sorter;
  GtkFilter *filter;
  gchar *full_name;
  GMenuModel *context_menu_model;

  StampAccount *account;
  CamelFolder *folder;
  gboolean selection_mode;

  StampCollectionItem *collection;
  gboolean is_collection;
  GList *collection_folders;

  enum FilterMode filter_mode;
  enum SortMode sort_mode;
  gboolean important_first;
  gboolean claimed;

  CamelFolderThread *thread;
  GHashTable *thread_cache;
  guint cache_tick;
  GtkFilterListModel *filter_model;
  guint anchor_position;
  GHashTable *bound_list_items;
  GPtrArray *trash_array;
  GList *trash_entries;

  GSimpleActionGroup *actions;
  GMenu *move_menu;
  guint load_folder_handler;
};

G_DEFINE_FINAL_TYPE (StampConversationList, stamp_conversation_list, ADW_TYPE_BIN);

typedef enum {
  PROP_STATE = 1,
} StampConversationListProps;

static GParamSpec *properties[PROP_STATE + 1];

enum {
  CONVERSATION_SELECTED,
  CONVERSATION_TRASH,
  LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = { 0 };

static gboolean
find_uid_in_thread_node (CamelFolderThreadNode *node,
                         const gchar           *uid)
{
  CamelMessageInfo *info;

  if (!node || !uid)
    return FALSE;

  info = camel_folder_thread_node_get_item (node);
  if (info && g_strcmp0 (camel_message_info_get_uid (info), uid) == 0)
    return TRUE;

  for (CamelFolderThreadNode *child = camel_folder_thread_node_get_child (node); child; child = camel_folder_thread_node_get_next (child)) {
    if (find_uid_in_thread_node (child, uid))
      return TRUE;
  }

  return FALSE;
}

static StampConversationItem *
stamp_conversation_item_find_item (GListStore  *store,
                                   const gchar *uid)
{
  guint list_len = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (guint idx = 0; idx < list_len; idx++) {
    g_autoptr (StampConversationItem) item = g_list_model_get_item (G_LIST_MODEL (store), idx);

    if (item && find_uid_in_thread_node (stamp_conversation_item_get_node (item), uid))
      return g_steal_pointer (&item);
  }
  return NULL;
}

static void
load_folder_idle (gpointer user_data);

static void
update_folder_idle (gpointer user_data);

static void
load_collection_idle (gpointer user_data);

static void
on_collection_folder_changed (CamelFolder           *folder,
                              CamelFolderChangeInfo *changes,
                              gpointer               user_data);

static void
on_collection_sources_changed (StampCollectionItem *collection,
                               gpointer             user_data);


static void
on_conversation_list_folder_changed (CamelFolder           *folder,
                                     CamelFolderChangeInfo *changes,
                                     gpointer               user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  if (self->full_name)
    g_hash_table_remove (self->thread_cache, self->full_name);

  if (changes->uid_changed && changes->uid_changed->len > 0) {
    GPtrArray *changed = changes->uid_changed;
    for (gint idx = 0; idx < changed->len; idx++) {
      g_autoptr (StampConversationItem) item = stamp_conversation_item_find_item (self->list_store, changed->pdata[idx]);
      CamelMessageInfo *message_info;
      gchar *uid = (char *)changed->pdata[idx];

      message_info = camel_folder_summary_get (camel_folder_get_folder_summary (folder), uid);

      if (item)
        stamp_conversation_item_update (item, message_info);
    }
  }

  if ((changes->uid_added && changes->uid_added->len > 0) || (changes->uid_removed && changes->uid_removed->len > 0)) {
    g_clear_handle_id (&self->load_folder_handler, g_source_remove);
    self->load_folder_handler = g_idle_add_once (update_folder_idle, self);
  }
}

static CamelFolderThread *
get_combined_thread (StampAccount *account,
                     CamelFolder  *folder)
{
  CamelFolder *sent_folder;
  g_autoptr (GPtrArray) items = NULL;
  g_autoptr (GPtrArray) uids = NULL;
  g_autoptr (GPtrArray) sent_uids = NULL;

  sent_folder = stamp_account_get_mail_sent_folder (account);
  if (!sent_folder || sent_folder == folder)
    return camel_folder_thread_new (folder, NULL, CAMEL_FOLDER_THREAD_FLAG_SORT);

  items = g_ptr_array_new_full (0, g_object_unref);

  camel_folder_summary_prepare_fetch_all (camel_folder_get_folder_summary (folder), NULL);
  camel_folder_summary_prepare_fetch_all (camel_folder_get_folder_summary (sent_folder), NULL);

  uids = camel_folder_dup_uids (folder);
  for (guint i = 0; i < uids->len; i++) {
    CamelMessageInfo *info = camel_folder_get_message_info (folder, uids->pdata[i]);

    if (info)
      g_ptr_array_add (items, info);
  }

  sent_uids = camel_folder_dup_uids (sent_folder);
  for (guint i = 0; i < sent_uids->len; i++) {
    CamelMessageInfo *info = camel_folder_get_message_info (sent_folder, sent_uids->pdata[i]);

    if (info)
      g_ptr_array_add (items, info);
  }

  if (items->len == 0)
    return NULL;

  return camel_folder_thread_new_items (items,
                                        CAMEL_FOLDER_THREAD_FLAG_SORT,
                                        (CamelFolderThreadStrFunc)camel_message_info_get_uid,
                                        (CamelFolderThreadStrFunc)camel_message_info_get_subject,
                                        (CamelFolderThreadUint64Func)camel_message_info_get_message_id,
                                        (CamelFolderThreadArrayFunc)camel_message_info_get_references,
                                        (CamelFolderThreadInt64Func)camel_message_info_get_date_sent,
                                        (CamelFolderThreadInt64Func)camel_message_info_get_date_received,
                                        (CamelFolderThreadVoidFunc)camel_message_info_property_lock,
                                        (CamelFolderThreadVoidFunc)camel_message_info_property_unlock);
}


static gboolean
node_has_message_from_folder (CamelFolderThreadNode *node,
                              CamelFolder           *folder)
{
  const CamelMessageInfo *info;

  if (!node)
    return FALSE;

  info = camel_folder_thread_node_get_item (node);
  if (info) {
    g_autoptr (CamelFolderSummary) summary = NULL;
    CamelFolder *msg_folder;

    g_object_get (G_OBJECT (info), "summary", &summary, NULL);

    msg_folder = camel_folder_summary_get_folder (summary);
    if (msg_folder == folder)
      return TRUE;
  }

  for (CamelFolderThreadNode *child = camel_folder_thread_node_get_child (node); child; child = camel_folder_thread_node_get_next (child)) {
    if (node_has_message_from_folder (child, folder))
      return TRUE;
  }

  return FALSE;
}

static gint
sorter_func (gconstpointer a,
             gconstpointer b,
             gpointer      user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationItem *item1 = (StampConversationItem *)(a);
  StampConversationItem *item2 = (StampConversationItem *)(b);
  guint timestamp1 = stamp_conversation_item_get_timestamp (item1);
  guint timestamp2 = stamp_conversation_item_get_timestamp (item2);
  gint64 uid1;
  gint64 uid2;

  if (self->important_first) {
    if (stamp_conversation_item_is_important (item1) && !stamp_conversation_item_is_important (item2)) {
      return -1;
    }
    if (!stamp_conversation_item_is_important (item1) && stamp_conversation_item_is_important (item2)) {
      return 1;
    }
  }

  if (G_UNLIKELY (timestamp1 != timestamp2)) {
    if (self->sort_mode == SORT_MODE_OLDEST_FIRST)
      return (timestamp1 - timestamp2);

    return -(timestamp1 - timestamp2);
  }

  uid1 = stamp_conversation_item_get_uid_number (item1);
  uid2 = stamp_conversation_item_get_uid_number (item2);
  return uid1 > uid2 ? -1 : 1;
}

static void
on_important_first_changed (GSettings   *settings,
                            const gchar *key,
                            gpointer     user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  self->important_first = g_settings_get_boolean (settings, key);
  gtk_sorter_changed (self->sorter, GTK_SORTER_CHANGE_DIFFERENT);
}

static gboolean
filter_func (gpointer object,
             gpointer user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationItem *item = STAMP_CONVERSATION_ITEM (object);
  const gchar *search_text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  gint search_text_len = search_text ? strlen (search_text) : 0;

  if (stamp_conversation_item_get_hidden (item))
    return FALSE;

  if (search_text_len) {
    g_autofree char *search = g_utf8_strdown (search_text, search_text_len);
    g_auto (GStrv) words = g_strsplit (search, " ", -1);
    gboolean all_found = TRUE;

    for (gint i = 0; words[i] && all_found; i++) {
      gboolean word_found = FALSE;

      if (strlen (words[i]) == 0)
        continue;

      if (!word_found) {
        const gchar *item_from = stamp_conversation_item_get_from (item);

        if (item_from && strlen (item_from) > 0) {
          g_autofree char *from = g_utf8_strdown (item_from, -1);

          if (g_strstr_len (from, -1, words[i]))
            word_found = TRUE;
        }
      }

      if (!word_found) {
        const gchar *item_subject = stamp_conversation_item_get_subject (item);

        if (item_subject && strlen (item_subject) > 0) {
          g_autofree char *subject = g_utf8_strdown (item_subject, -1);

          if (g_strstr_len (subject, -1, words[i]))
            word_found = TRUE;
        }
      }

      if (!word_found) {
        const gchar *item_mail = stamp_conversation_item_get_mail (item);

        if (item_mail && strlen (item_mail) > 0) {
          g_autofree char *mail = g_utf8_strdown (item_mail, -1);

          if (g_strstr_len (mail, -1, words[i]))
            word_found = TRUE;
        }
      }

      if (!word_found) {
        const gchar *item_preview = stamp_conversation_item_get_preview (item);

        if (item_preview && strlen (item_preview) > 0) {
          g_autofree char *preview = g_utf8_strdown (item_preview, -1);

          if (g_strstr_len (preview, -1, words[i]))
            word_found = TRUE;
        }
      }

      if (!word_found)
        all_found = FALSE;
    }

    if (!all_found)
      return FALSE;
  }

  switch (self->filter_mode) {
    case FILTER_MODE_UNREAD:
      return stamp_conversation_item_get_unread (item) != 0;
    case FILTER_MODE_STARRED:
      return stamp_conversation_item_get_flagged (item);
    case FILTER_MODE_ATTACHMENT:
      return stamp_conversation_item_has_attachment (item);
    case FILTER_MODE_ALL:
    default:
      break;
  }

  return TRUE;
}

static void
on_single_selection_changed (GtkSelectionModel *model,
                             guint              position,
                             guint              n_items,
                             gpointer           user_data);

static void
on_category_toggled (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationItem *item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (self->single_selection));
  g_autoptr (GVariant) state = g_action_get_state (G_ACTION (action));
  gboolean active = g_variant_get_boolean (state);
  const gchar *action_name = g_action_get_name (G_ACTION (action));
  const gchar *id = action_name + 4;

  g_simple_action_set_state (action, g_variant_new_boolean (!active));
  stamp_conversation_item_set_label (item, id, !active);
}

static void
rebuild_category_actions (StampConversationList *self)
{
  GActionMap *map = G_ACTION_MAP (self->actions);
  gchar **old_names = g_object_get_data (G_OBJECT (self->actions), "cat-action-names");
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GList *categories;

  if (!self->account)
    return;

  categories = stamp_account_get_categories (self->account);

  if (old_names) {
    for (guint idx = 0; old_names[idx]; idx++)
      g_action_map_remove_action (map, old_names[idx]);
  }

  for (GList *iter = categories; iter && iter->data; iter = g_list_next (iter)) {
    StampCategory *cat = iter->data;
    g_autoptr (GString) name = g_string_new (stamp_category_get_name (cat));
    gchar *action_name;
    g_autoptr (GSimpleAction) action;

    g_string_replace (name, " ", "_", 0);

    action_name = g_strdup_printf ("cat-%s", name->str);
    action = g_simple_action_new_stateful (action_name, NULL, g_variant_new_boolean (FALSE));

    g_signal_connect (action, "activate", G_CALLBACK (on_category_toggled), self);
    g_action_map_add_action (map, G_ACTION (action));

    g_ptr_array_add (names, g_strdup (action_name));
  }

  g_ptr_array_add (names, NULL);
  g_object_set_data_full (G_OBJECT (self->actions), "cat-action-names", g_ptr_array_free (names, FALSE), (GDestroyNotify)g_strfreev);
}

static void
on_items_changed (GListModel *model,
                  guint       position,
                  guint       removed,
                  guint       added,
                  gpointer    user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  guint n_items = g_list_model_get_n_items (model);

  if (n_items == 0) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->mail_list_stack), "empty");
  } else {
    gtk_stack_set_visible_child_name (GTK_STACK (self->mail_list_stack), "list");
  }

  rebuild_category_actions (self);
}

static void
append_folder_items (GMenu           *menu,
                     CamelFolderInfo *fi,
                     const gchar     *current_folder)
{
  GMenu *section = g_menu_new ();

  while (fi) {
    if (fi->flags & (CAMEL_FOLDER_NOSELECT | CAMEL_FOLDER_VIRTUAL)) {
      fi = fi->next;
      continue;
    }

    if (current_folder && g_strcmp0 (fi->full_name, current_folder) == 0) {
      if (fi->child)
        append_folder_items (menu, fi->child, current_folder);

      fi = fi->next;
      continue;
    }

    if (fi->child) {
      GMenu *submenu = g_menu_new ();

      append_folder_items (submenu, fi->child, current_folder);
      g_menu_append_submenu (section, fi->display_name, G_MENU_MODEL (submenu));
    } else {
      g_autofree gchar *action = g_strdup_printf ("conversation-list.move-folder::%s", fi->full_name);

      g_menu_append (section, fi->display_name, action);
    }

    fi = fi->next;
  }

  if (g_menu_model_get_n_items (G_MENU_MODEL (section)) > 0)
    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
  else
    g_clear_object (&section);
}

static void
on_folder_info_for_move_menu (GObject      *src,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelFolderInfo) root = camel_store_get_folder_info_finish (CAMEL_STORE (src), res, &error);

  if (error) {
    g_warning ("Could not get folder info: %s", error->message);

    self->move_menu = g_menu_new ();
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (self->move_selection_button), G_MENU_MODEL (self->move_menu));
    return;
  }

  self->move_menu = g_menu_new ();

  if (root)
    append_folder_items (self->move_menu, root, self->full_name);

  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (self->move_selection_button), G_MENU_MODEL (self->move_menu));
}

static void
rebuild_move_menu (StampConversationList *self)
{
  StampMailService *mail_service;
  CamelStore *store;

  if (!self->account)
    return;

  g_clear_object (&self->move_menu);

  mail_service = stamp_account_get_mail_service (self->account);
  store = CAMEL_STORE (stamp_mail_service_get_service (mail_service));

  camel_store_get_folder_info (store, NULL, CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_NO_VIRTUAL, G_PRIORITY_DEFAULT, NULL, on_folder_info_for_move_menu, self);
}

typedef struct {
  StampAccount *account;
  CamelFolder *folder;
  CamelFolderThread *cached_thread;
  gchar *service_uid;
} ThreadBuildData;

typedef struct {
  CamelFolderThread *thread;
  GPtrArray *items;
} ThreadResult;

static void
thread_result_free (gpointer user_data)
{
  ThreadResult *result = user_data;

  g_clear_object (&result->thread);
  g_clear_pointer (&result->items, g_ptr_array_unref);
  g_free (result);
}

static GPtrArray *
build_items (CamelFolderThread *thread,
             CamelFolder       *folder,
             const gchar       *service_uid)
{
  GPtrArray *array = g_ptr_array_new_with_free_func (g_object_unref);

  if (thread) {
    CamelFolderThreadNode *child = camel_folder_thread_get_tree (thread);

    while (child) {
      StampConversationItem *item = stamp_conversation_item_new (thread, child, service_uid);

      if (!node_has_message_from_folder (child, folder))
        stamp_conversation_item_set_hidden (item, TRUE);

      g_ptr_array_add (array, item);
      child = camel_folder_thread_node_get_next (child);
    }
  }

  return array;
}

static void
thread_build_data_free (gpointer user_data)
{
  ThreadBuildData *data = user_data;

  g_clear_object (&data->account);
  g_clear_object (&data->folder);
  g_clear_object (&data->cached_thread);
  g_free (data->service_uid);
  g_free (data);
}

static void
thread_build_task (GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data,
                   GCancellable *cancellable)
{
  ThreadBuildData *data = task_data;
  ThreadResult *result;

  if (g_task_return_error_if_cancelled (task))
    return;

  result = g_new0 (ThreadResult, 1);
  result->thread = data->cached_thread ? g_object_ref (data->cached_thread) : get_combined_thread (data->account, data->folder);
  result->items = build_items (result->thread, data->folder, data->service_uid);

  g_task_return_pointer (task, result, thread_result_free);
}

typedef struct {
  StampConversationList *self;
  CamelFolder *folder;
  CamelFolder *sent_folder;
  CamelFolderThread *thread;
  gchar *full_name;
  guint tick;
  gulong folder_changed_id;
  gulong sent_changed_id;
} CachedThread;

static void
on_cached_folder_changed (CamelFolder           *folder,
                          CamelFolderChangeInfo *changes,
                          gpointer               user_data)
{
  CachedThread *entry = user_data;
  StampConversationList *self = entry->self;

  if (!self->thread_cache)
    return;

  if (entry->sent_folder && folder == entry->sent_folder)
    g_hash_table_remove_all (self->thread_cache);
  else
    g_hash_table_remove (self->thread_cache, entry->full_name);
}

static void
cached_thread_free (gpointer data)
{
  CachedThread *entry = data;

  if (entry->folder && entry->folder_changed_id)
    g_clear_signal_handler (&entry->folder_changed_id, entry->folder);

  if (entry->sent_folder && entry->sent_changed_id)
    g_clear_signal_handler (&entry->sent_changed_id, entry->sent_folder);

  g_clear_object (&entry->folder);
  g_clear_object (&entry->sent_folder);
  g_clear_object (&entry->thread);
  g_clear_pointer (&entry->full_name, g_free);
  g_free (entry);
}

static void
cache_thread (StampConversationList *self,
              const gchar           *full_name,
              CamelFolder           *folder,
              CamelFolderThread     *thread)
{
  CamelFolder *sent_folder;
  CachedThread *entry;

  if (!full_name || !folder)
    return;

  g_hash_table_remove (self->thread_cache, full_name);

  if (g_hash_table_size (self->thread_cache) >= THREAD_CACHE_MAX) {
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    CachedThread *oldest = NULL;
    const gchar *oldest_key = NULL;

    g_hash_table_iter_init (&iter, self->thread_cache);

    while (g_hash_table_iter_next (&iter, &key, &value)) {
      CachedThread *candidate = value;

      if (!oldest || candidate->tick < oldest->tick) {
        oldest = candidate;
        oldest_key = key;
      }
    }

    if (oldest_key)
      g_hash_table_remove (self->thread_cache, oldest_key);
  }

  sent_folder = stamp_account_get_mail_sent_folder (self->account);

  entry = g_new0 (CachedThread, 1);
  entry->self = self;
  entry->folder = g_object_ref (folder);
  entry->thread = thread ? g_object_ref (thread) : NULL;
  entry->full_name = g_strdup (full_name);
  entry->tick = ++self->cache_tick;
  entry->folder_changed_id = g_signal_connect (folder, "changed", G_CALLBACK (on_cached_folder_changed), entry);

  if (sent_folder && sent_folder != folder) {
    entry->sent_folder = g_object_ref (sent_folder);
    entry->sent_changed_id = g_signal_connect (sent_folder, "changed", G_CALLBACK (on_cached_folder_changed), entry);
  }

  g_hash_table_insert (self->thread_cache, g_strdup (full_name), entry);
}

static gboolean
items_match (StampConversationItem *a,
             StampConversationItem *b)
{
  return g_strcmp0 (stamp_conversation_item_get_uid (a), stamp_conversation_item_get_uid (b)) == 0 &&
         stamp_conversation_item_get_num_messages (a) == stamp_conversation_item_get_num_messages (b) &&
         stamp_conversation_item_get_timestamp (a) == stamp_conversation_item_get_timestamp (b);
}

/* Only replaces the differing range so the list view keeps its scroll position */
static void
splice_keeping_unchanged (StampConversationList *self,
                          GPtrArray             *array)
{
  GListModel *model = G_LIST_MODEL (self->list_store);
  guint old_n = g_list_model_get_n_items (model);
  guint prefix = 0;
  guint suffix = 0;

  while (prefix < old_n && prefix < array->len) {
    g_autoptr (StampConversationItem) old_item = g_list_model_get_item (model, prefix);

    if (!items_match (old_item, g_ptr_array_index (array, prefix)))
      break;
    prefix++;
  }

  while (suffix < old_n - prefix && suffix < array->len - prefix) {
    g_autoptr (StampConversationItem) old_item = g_list_model_get_item (model, old_n - 1 - suffix);

    if (!items_match (old_item, g_ptr_array_index (array, array->len - 1 - suffix)))
      break;
    suffix++;
  }

  if (prefix + suffix == old_n && old_n == array->len)
    return;

  g_list_store_splice (self->list_store, prefix, old_n - prefix - suffix,
                       array->len > prefix ? array->pdata + prefix : NULL, array->len - prefix - suffix);
}

static void
display_thread (StampConversationList *self,
                CamelFolderThread     *thread,
                GPtrArray             *array)
{
  StampMailService *mail_service;
  CamelFolder *folder = self->folder;
  const gchar *service_uid;

  gboolean keep_scroll = self->keep_scroll;

  self->keep_scroll = TRUE;
  g_set_object (&self->thread, thread);

  mail_service = stamp_account_get_mail_service (self->account);
  service_uid = camel_service_get_uid (stamp_mail_service_get_service (mail_service));
  g_hash_table_insert (self->folders, g_strdup (service_uid), g_object_ref (folder));

  if (keep_scroll)
    splice_keeping_unchanged (self, array);
  else
    g_list_store_splice (self->list_store, 0, g_list_model_get_n_items (G_LIST_MODEL (self->list_store)), (gpointer *)array->pdata, array->len);

  if (self->window_title) {
    adw_window_title_set_title (ADW_WINDOW_TITLE (self->window_title), camel_folder_get_display_name (folder));
    adw_window_title_set_subtitle (ADW_WINDOW_TITLE (self->window_title), stamp_account_get_name (self->account));
  }

  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_widget_set_margin_top (self->spinner, 12);

  if (!keep_scroll && g_list_model_get_n_items (G_LIST_MODEL (gtk_list_view_get_model (GTK_LIST_VIEW (self->listview)))) > 0)
    gtk_list_view_scroll_to (GTK_LIST_VIEW (self->listview), 0, GTK_LIST_SCROLL_FOCUS, NULL);

  rebuild_move_menu (self);
}

static void
on_thread_built (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  GTask *task = G_TASK (result);
  ThreadBuildData *data = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;
  ThreadResult *built;

  built = g_task_propagate_pointer (task, &error);

  if (error || data->folder != self->folder ||
      g_task_get_cancellable (task) != self->thread_cancellable) {
    g_clear_pointer (&built, thread_result_free);

    if (g_task_get_cancellable (task) == self->thread_cancellable)
      gtk_widget_set_visible (self->spinner, FALSE);

    return;
  }

  display_thread (self, built->thread, built->items);
  cache_thread (self, self->full_name, self->folder, built->thread);
  thread_result_free (built);
}

static void
rebuild_list (StampConversationList *self)
{
  CachedThread *cached;
  ThreadBuildData *data;
  StampMailService *mail_service;
  g_autoptr (GTask) task = NULL;

  if (!self->folder || !self->account)
    return;

  cached = self->full_name ? g_hash_table_lookup (self->thread_cache, self->full_name) : NULL;
  if (cached && cached->folder == self->folder)
    cached->tick = ++self->cache_tick;
  else
    cached = NULL;

  gtk_stack_set_visible_child_name (GTK_STACK (self->mail_list_stack), "list");
  gtk_widget_set_visible (self->spinner, TRUE);

  g_cancellable_cancel (self->thread_cancellable);
  g_clear_object (&self->thread_cancellable);
  self->thread_cancellable = g_cancellable_new ();

  data = g_new0 (ThreadBuildData, 1);
  data->account = g_object_ref (self->account);
  data->folder = g_object_ref (self->folder);
  data->cached_thread = cached && cached->thread ? g_object_ref (cached->thread) : NULL;
  mail_service = stamp_account_get_mail_service (self->account);
  data->service_uid = g_strdup (camel_service_get_uid (stamp_mail_service_get_service (mail_service)));

  task = g_task_new (self, self->thread_cancellable, on_thread_built, self);
  g_task_set_source_tag (task, rebuild_list);
  g_task_set_task_data (task, data, thread_build_data_free);
  g_task_run_in_thread (task, thread_build_task);
}

static void
on_folder_refreshed (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  CamelFolder *folder = CAMEL_FOLDER (source);
  g_autoptr (GError) error = NULL;

  if (!camel_folder_refresh_info_finish (folder, res, &error))
    g_warning ("Could not refresh folder %s: %s", camel_folder_get_display_name (folder), error->message);

  if (folder == self->folder && !self->is_collection)
    rebuild_list (self);
}

static void
on_get_folder (GObject      *source,
               GAsyncResult *res,
               gpointer      user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  CamelStore *store = CAMEL_STORE (source);
  g_autoptr (CamelFolder) folder = NULL;
  g_autoptr (GError) error = NULL;

  folder = camel_store_get_folder_finish (store, res, &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  if (self->folder) {
    g_signal_handlers_disconnect_by_func (self->folder, on_conversation_list_folder_changed, self);
    g_clear_object (&self->folder);
  }

  if (error) {
    g_warning ("Could not get folder: %s", error->message);
    gtk_widget_set_visible (self->spinner, FALSE);
    gtk_widget_set_margin_top (self->spinner, 12);

    rebuild_category_actions (self);
    return;
  }

  self->folder = g_object_ref (folder);
  g_signal_connect_object (folder, "changed", G_CALLBACK (on_conversation_list_folder_changed), self, G_CONNECT_DEFAULT);

  rebuild_list (self);
}

void
stamp_conversation_list_load_folder (StampConversationList *self,
                                     StampAccount          *account,
                                     gchar                 *full_name)
{
  StampMailService *mail_service;

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  g_clear_handle_id (&self->load_folder_handler, g_source_remove);
  self->keep_scroll = FALSE;

  if (!account)
    return;

  self->is_collection = FALSE;
  if (self->collection)
    g_signal_handlers_disconnect_by_func (self->collection, on_collection_sources_changed, self);
  g_clear_object (&self->collection);
  self->cancellable = g_cancellable_new ();

  mail_service = stamp_account_get_mail_service (account);
  self->account = account;

  g_set_str (&self->full_name, full_name);

  camel_store_get_folder (CAMEL_STORE (stamp_mail_service_get_service (mail_service)), full_name, CAMEL_STORE_FOLDER_NONE, G_PRIORITY_DEFAULT, self->cancellable, on_get_folder, self);
}

void
stamp_conversation_list_refresh (StampConversationList *self)
{
  if (!self->folder)
    return;

  self->keep_scroll = TRUE;
  camel_folder_refresh_info (self->folder, G_PRIORITY_DEFAULT, self->cancellable, on_folder_refreshed, self);
}

typedef struct {
  StampConversationList *self;
  GList *sources;
  guint pending;
} LoadCollectionData;

static void
load_collection_data_free (gpointer user_data)
{
  LoadCollectionData *data = user_data;

  if (!data)
    return;

  g_list_free (data->sources);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (LoadCollectionData, load_collection_data_free)

static void
collection_folder_done (LoadCollectionData *data)
{
  StampConversationList *self = data->self;

  data->pending--;

  if (data->pending == 0) {
    gtk_widget_set_visible (self->spinner, FALSE);
    gtk_widget_set_margin_top (self->spinner, 12);

    if (g_list_model_get_n_items (G_LIST_MODEL (self->list_store)) > 0)
      gtk_list_view_scroll_to (GTK_LIST_VIEW (self->listview), 0, GTK_LIST_SCROLL_FOCUS, NULL);

    load_collection_data_free (data);
  }
}

static void
on_collection_thread_built (GObject      *source_object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  LoadCollectionData *data = user_data;
  StampConversationList *self = data->self;
  g_autoptr (GError) error = NULL;
  ThreadResult *built;

  built = g_task_propagate_pointer (G_TASK (result), &error);

  if (built) {
    g_list_store_splice (self->list_store,
                         g_list_model_get_n_items (G_LIST_MODEL (self->list_store)),
                         0,
                         (gpointer *)built->items->pdata,
                         built->items->len);
    thread_result_free (built);
  }

  collection_folder_done (data);
}

static void
on_get_collection_folder (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  LoadCollectionData *data = user_data;
  StampConversationList *self = data->self;
  CamelStore *store = CAMEL_STORE (source);
  g_autoptr (CamelFolder) folder = NULL;
  g_autoptr (GError) error = NULL;
  ThreadBuildData *build;
  g_autoptr (GTask) task = NULL;

  folder = camel_store_get_folder_finish (store, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Could not get collection folder: %s", error->message);

    collection_folder_done (data);
    return;
  }

  self->collection_folders = g_list_prepend (self->collection_folders, g_object_ref (folder));
  g_signal_connect_object (folder, "changed", G_CALLBACK (on_collection_folder_changed), self, G_CONNECT_DEFAULT);

  build = g_new0 (ThreadBuildData, 1);
  build->account = g_object_ref (STAMP_ACCOUNT (g_object_get_data (source, "collection-account")));
  build->folder = g_object_ref (folder);
  build->service_uid = g_strdup (camel_service_get_uid (CAMEL_SERVICE (store)));

  task = g_task_new (NULL, self->cancellable, on_collection_thread_built, data);
  g_task_set_source_tag (task, on_get_collection_folder);
  g_task_set_task_data (task, build, thread_build_data_free);
  g_task_run_in_thread (task, thread_build_task);
}

static void
load_collection_idle (gpointer user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  stamp_conversation_list_load_collection (self, self->collection);
  self->load_folder_handler = 0;
}

static void
on_collection_folder_changed (CamelFolder           *folder,
                              CamelFolderChangeInfo *changes,
                              gpointer               user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  if (changes->uid_changed && changes->uid_changed->len > 0) {
    GPtrArray *changed = changes->uid_changed;

    for (gint idx = 0; idx < changed->len; idx++) {
      g_autoptr (StampConversationItem) item = stamp_conversation_item_find_item (self->list_store, changed->pdata[idx]);
      CamelMessageInfo *message_info;
      gchar *uid = (char *)changed->pdata[idx];

      message_info = camel_folder_summary_get (camel_folder_get_folder_summary (folder), uid);

      if (item)
        stamp_conversation_item_update (item, message_info);
    }
  }

  if ((changes->uid_added && changes->uid_added->len > 0) || (changes->uid_removed && changes->uid_removed->len > 0)) {
    g_clear_handle_id (&self->load_folder_handler, g_source_remove);
    self->load_folder_handler = g_idle_add_once (load_collection_idle, self);
  }
}

static void
on_collection_sources_changed (StampCollectionItem *collection,
                               gpointer             user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  if (!self->is_collection)
    return;

  g_clear_handle_id (&self->load_folder_handler, g_source_remove);
  self->load_folder_handler = g_idle_add_once (load_collection_idle, self);
}

void
stamp_conversation_list_load_collection (StampConversationList *self,
                                         StampCollectionItem   *collection)
{
  g_autoptr (LoadCollectionData) data = NULL;
  GList *iter;

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  g_clear_handle_id (&self->load_folder_handler, g_source_remove);

  for (iter = self->collection_folders; iter; iter = iter->next)
    g_signal_handlers_disconnect_by_func (iter->data, on_collection_folder_changed, self);

  g_list_free_full (g_steal_pointer (&self->collection_folders), g_object_unref);

  if (self->folder) {
    g_signal_handlers_disconnect_by_func (self->folder, on_conversation_list_folder_changed, self);
    g_clear_object (&self->folder);
  }

  g_list_store_splice (self->list_store,
                       0,
                       g_list_model_get_n_items (G_LIST_MODEL (self->list_store)),
                       NULL,
                       0);

  self->is_collection = TRUE;
  g_clear_object (&self->account);
  if (self->collection)
    g_signal_handlers_disconnect_by_func (self->collection, on_collection_sources_changed, self);
  g_set_object (&self->collection, collection);
  g_signal_connect_object (collection, "sources-changed", G_CALLBACK (on_collection_sources_changed), self, G_CONNECT_DEFAULT);
  g_clear_pointer (&self->full_name, g_free);

  self->cancellable = g_cancellable_new ();

  data = g_new0 (LoadCollectionData, 1);
  data->self = self;
  data->sources = g_list_copy (stamp_collection_item_get_sources (collection));

  for (iter = data->sources; iter; iter = iter->next) {
    StampCollectionSource *source = iter->data;
    StampMailService *mail_service = stamp_account_get_mail_service (source->account);

    if (!mail_service || !stamp_mail_service_get_enabled (mail_service))
      continue;

    data->pending++;

    g_object_set_data (G_OBJECT (stamp_mail_service_get_service (mail_service)),
                       "collection-account",
                       source->account);
    camel_store_get_folder (CAMEL_STORE (stamp_mail_service_get_service (mail_service)),
                            source->full_name,
                            CAMEL_STORE_FOLDER_NONE,
                            G_PRIORITY_DEFAULT,
                            self->cancellable,
                            on_get_collection_folder,
                            data);
  }

  if (data->pending == 0) {
    return;
  }

  /* The pending callbacks release the shared collection data. */
  g_steal_pointer (&data);
  gtk_widget_set_visible (self->spinner, TRUE);
}

StampAccount *
stamp_conversation_list_get_selected_account (StampConversationList *self)
{
  StampConversationItem *item;
  const gchar *service_uid;
  StampSession *session;

  if (!self->is_collection)
    return self->account;

  item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));
  if (!item)
    return NULL;

  service_uid = stamp_conversation_item_get_service_uid (item);
  if (!service_uid)
    return NULL;

  session = stamp_session_get_default ();

  return stamp_session_find_account_by_mail_uid (session, service_uid);
}

static StampAccount *
get_item_account (StampConversationList *self,
                  StampConversationItem *item)
{
  StampSession *session;
  const gchar *service_uid;

  if (!self->is_collection)
    return self->account;

  service_uid = stamp_conversation_item_get_service_uid (item);
  if (!service_uid)
    return NULL;

  session = stamp_session_get_default ();

  return stamp_session_find_account_by_mail_uid (session, service_uid);
}

static void
on_mail_search_entry_changed (GtkWidget *search_entry,
                              gpointer   user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  if (self->filter)
    gtk_filter_changed (self->filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static void
on_new_message (GObject  *button,
                gpointer  user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  GtkWidget *composer = stamp_composer_new (self->account);
  gtk_window_present (GTK_WINDOW (composer));
}

static void
on_row_mark_read (GtkWidget *widget,
                  gpointer   user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationRow *row = STAMP_CONVERSATION_ROW (widget);
  StampConversationItem *item = stamp_conversation_row_get_item (row);
  CamelFolderThreadNode *node;

  node = stamp_conversation_item_get_node (item);

  stamp_conversation_list_mark_read (self, node);
}

static void
on_row_mark_unread (GtkWidget *widget,
                    gpointer   user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationRow *row = STAMP_CONVERSATION_ROW (widget);
  StampConversationItem *item = stamp_conversation_row_get_item (row);
  CamelFolderThreadNode *node;

  node = stamp_conversation_item_get_node (item);

  stamp_conversation_list_mark_unread (self, node);
}

static void
on_row_trash (GtkWidget *widget,
              gpointer   user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationRow *row = STAMP_CONVERSATION_ROW (widget);
  StampConversationItem *item = stamp_conversation_row_get_item (row);

  g_signal_emit (self, signals[CONVERSATION_TRASH], 0, item);
}

typedef struct {
  StampConversationList *self;
  GtkListItem *list_item;
  gdouble start_x;
  gdouble start_y;
} RowData;

static void
handle_popover (RowData *row_data,
                gdouble  x,
                gdouble  y)
{
  GtkWidget *popover;
  g_autoptr (GMenu) menu = NULL;
  g_autoptr (GMenu) sub_menu = NULL;
  g_autoptr (GPtrArray) templates = NULL;
  GdkRectangle rect;
  StampConversationRow *child = STAMP_CONVERSATION_ROW (gtk_list_item_get_child (row_data->list_item));
  graphene_point_t src_point = { (float)x, (float)y };
  graphene_point_t dst_point;
  guint position = gtk_list_item_get_position (row_data->list_item);
  GtkSelectionModel *model = gtk_list_view_get_model (GTK_LIST_VIEW (row_data->self->listview));
  GList *categories;
  GPtrArray *labels;
  StampConversationItem *conversation_item;
  StampAccount *account;

  gtk_selection_model_select_item (model, position, TRUE);

  account = stamp_conversation_list_get_selected_account (row_data->self);

  menu = g_menu_new ();
  g_menu_append (menu, _("Reply"), "mail.reply-current");
  g_menu_append (menu, _("Reply All"), "mail.reply-all-current");

  if (account)
    templates = stamp_account_get_mail_templates (account);

  if (templates && templates->len > 0) {
    g_autoptr (GMenu) template_menu = g_menu_new ();

    for (guint i = 0; i < templates->len; i++) {
      CamelMessageInfo *info = templates->pdata[i];
      const gchar *subject = camel_message_info_get_subject (info);
      g_autoptr (GMenuItem) template_item = g_menu_item_new (subject && *subject ? subject : _("No Subject"), NULL);

      g_menu_item_set_action_and_target (template_item, "mail.reply-with-template", "s", camel_message_info_get_uid (info));
      g_menu_append_item (template_menu, template_item);
    }

    g_menu_append_submenu (menu, _("Reply with Template"), G_MENU_MODEL (template_menu));
  }

  g_menu_append (menu, _("Forward"), "mail.forward-current");

  conversation_item = stamp_conversation_row_get_item (child);

  if (stamp_conversation_item_get_unread (conversation_item))
    g_menu_append (menu, _("Mark Read"), "conversation-list.mark-read");
  else
    g_menu_append (menu, _("Mark Unread"), "conversation-list.mark-unread");

  if (stamp_conversation_item_get_flagged (conversation_item))
    g_menu_append (menu, _("Mark Unstarred"), "conversation-list.mark-unstarred");
  else
    g_menu_append (menu, _("Mark Starred"), "conversation-list.mark-starred");

  g_menu_append (menu, _("Move to Trash"), "mail.trash");

  labels = stamp_conversation_item_get_labels (STAMP_CONVERSATION_ITEM (gtk_list_item_get_item (row_data->list_item)));
  /* The combined inbox has no single account, and no category actions either. */
  categories = row_data->self->account ? stamp_account_get_categories (row_data->self->account) : NULL;
  if (categories) {
    sub_menu = g_menu_new ();

    for (GList *iter = categories; iter && iter->data; iter = g_list_next (iter)) {
      StampCategory *cat = iter->data;
      g_autoptr (GString) name = g_string_new (stamp_category_get_name (cat));
      GMenuItem *item;
      g_autofree char *action_name = NULL;
      g_autofree char *full_action_name = NULL;
      GAction *action;

      g_string_replace (name, " ", "_", 0);
      action_name = g_strdup_printf ("cat-%s", name->str);
      full_action_name = g_strdup_printf ("conversation-list.%s", action_name);
      item = g_menu_item_new (stamp_category_get_name (cat), full_action_name);
      g_menu_append_item (sub_menu, item);

      action = g_action_map_lookup_action (G_ACTION_MAP (row_data->self->actions), action_name);
      g_simple_action_set_state (G_SIMPLE_ACTION (action), g_variant_new_boolean (FALSE));
      for (gint idx = 0; idx < labels->len; idx++) {
        gchar *cat_name = labels->pdata[idx];

        if (g_strcmp0 (cat_name, action_name + 4) == 0) {
          g_simple_action_set_state (G_SIMPLE_ACTION (action), g_variant_new_boolean (TRUE));
          break;
        }
      }
    }

    g_menu_append_submenu (menu, _("Category"), G_MENU_MODEL (sub_menu));
  }

  popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
  gtk_widget_set_halign (popover, GTK_ALIGN_START);
  gtk_widget_set_valign (popover, GTK_ALIGN_START);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_widget_set_parent (popover, row_data->self->listview);

  if (!gtk_widget_compute_point (GTK_WIDGET (child), row_data->self->listview, &src_point, &dst_point)) {
    g_warning ("%s: compute point failed", G_STRFUNC);
    return;
  }

  rect.x = dst_point.x;
  rect.y = dst_point.y;
  rect.width = 1;
  rect.height = 1;
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);

  gtk_popover_popup (GTK_POPOVER (popover));
}

static void
on_released (GtkEventController *controller,
             gint                n_press,
             gdouble             x,
             gdouble             y,
             gpointer            user_data)
{
  RowData *row_data = g_object_get_data (G_OBJECT (controller), "row-data");

  handle_popover (row_data, x, y);
}

static void
update_selection_title (StampConversationList *self)
{
  guint n_selected = gtk_bitset_get_size (self->selected);
  gchar buf[64];

  if (n_selected) {
    g_autofree char *tooltip = g_strdup_printf (g_dngettext (NULL, "Move Selected Message to Trash", "Move %u Selected Messages to Trash", n_selected), n_selected);

    gtk_widget_set_tooltip_text (GTK_WIDGET (self->trash_selection_button), tooltip);
    gtk_widget_set_sensitive (GTK_WIDGET (self->trash_selection_button), TRUE);
  } else {
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->trash_selection_button), NULL);
    gtk_widget_set_sensitive (GTK_WIDGET (self->trash_selection_button), FALSE);
  }

  if (n_selected) {
    g_autofree char *tooltip = g_strdup_printf (g_dngettext (NULL, "Mark Selected Message as Read", "Mark %u Selected Messages as Read", n_selected), n_selected);

    gtk_widget_set_tooltip_text (GTK_WIDGET (self->mark_read_selection_button), tooltip);
    gtk_widget_set_sensitive (GTK_WIDGET (self->mark_read_selection_button), TRUE);
  } else {
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->mark_read_selection_button), NULL);
    gtk_widget_set_sensitive (GTK_WIDGET (self->mark_read_selection_button), FALSE);
  }

  if (n_selected) {
    g_autofree char *tooltip = g_strdup_printf (g_dngettext (NULL, "Move Selected Message…", "Move %u Selected Messages…", n_selected), n_selected);

    gtk_widget_set_tooltip_text (GTK_WIDGET (self->move_selection_button), tooltip);
    gtk_widget_set_sensitive (GTK_WIDGET (self->move_selection_button), TRUE);
  } else {
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->move_selection_button), NULL);
    gtk_widget_set_sensitive (GTK_WIDGET (self->move_selection_button), FALSE);
  }

  g_snprintf (buf, sizeof buf, _("%u selected"), n_selected);
  adw_window_title_set_title (ADW_WINDOW_TITLE (self->selection_label), buf);
}

static void
set_selection_active (StampConversationList *self,
                      gboolean               selection_active)
{
  self->selection_mode = selection_active;

  if (self->selection_mode) {
    gtk_bitset_remove_all (self->selected);
    update_selection_title (self);
    gtk_stack_set_visible_child (GTK_STACK (self->header_stack), self->selection_headerbar);
    gtk_list_view_set_model (GTK_LIST_VIEW (self->listview), GTK_SELECTION_MODEL (self->multi_selection));
    gtk_widget_set_visible (self->new_message, FALSE);
    gtk_widget_set_visible (self->selection_bottom_bar, TRUE);
  } else {
    gtk_stack_set_visible_child (GTK_STACK (self->header_stack), self->normal_headerbar);
    gtk_list_view_set_model (GTK_LIST_VIEW (self->listview), GTK_SELECTION_MODEL (self->single_selection));
    gtk_widget_set_visible (self->new_message, TRUE);
    gtk_widget_set_visible (self->selection_bottom_bar, FALSE);
    stamp_conversation_list_unselect (self);
  }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATE]);
}

static void
on_selection_changed (GtkCheckButton *check,
                      GParamSpec     *pspec,
                      GtkListItem    *list_item);

static void
refresh_checkboxes (StampConversationList *self)
{
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func (g_object_unref);
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, self->bound_list_items);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (items, g_object_ref (key));

  for (guint i = 0; i < items->len; i++) {
    GtkListItem *list_item = g_ptr_array_index (items, i);
    GtkWidget *row;
    g_autoptr (GtkCheckButton) check = NULL;
    guint position;

    if (!g_hash_table_contains (self->bound_list_items, list_item))
      continue;

    position = gtk_list_item_get_position (list_item);
    if (position == GTK_INVALID_LIST_POSITION)
      continue;

    row = gtk_list_item_get_child (list_item);
    check = g_object_ref (stamp_conversation_row_get_check_button (STAMP_CONVERSATION_ROW (row)));
    gtk_check_button_set_active (check, gtk_bitset_contains (self->selected, position));
  }
}

static void
on_long_press_pressed (GtkGestureLongPress *controller,
                       gdouble              x,
                       gdouble              y,
                       gpointer             user_data)
{
  RowData *row_data = g_object_get_data (G_OBJECT (controller), "row-data");
  StampConversationList *self = STAMP_CONVERSATION_LIST (row_data->self);
  guint position = gtk_list_item_get_position (row_data->list_item);

  set_selection_active (self, TRUE);

  gtk_bitset_add (self->selected, position);
  update_selection_title (self);
  refresh_checkboxes (self);
}

static void
on_row_pressed (GtkGestureClick *gesture,
                gint             n_press,
                gdouble          x,
                gdouble          y,
                gpointer         user_data)
{
  RowData *data = g_object_get_data (G_OBJECT (gesture), "row-data");
  StampConversationList *self = data->self;
  GtkListItem *list_item = data->list_item;
  guint position = gtk_list_item_get_position (list_item);
  GdkEvent *event = gtk_gesture_get_last_event (GTK_GESTURE (gesture), NULL);
  GdkModifierType state;
  gboolean ctrl;
  gboolean shift;

  if (!event || gdk_event_get_event_type (event) != GDK_BUTTON_PRESS)
    return;

  if (!self->selection_mode) {
    if (n_press == 2) {
      GtkWidget *window = adw_window_new ();
      GtkWidget *message_list = stamp_message_list_new ();
      StampConversationItem *item = STAMP_CONVERSATION_ITEM (gtk_list_item_get_item (list_item));

      stamp_message_list_set_conversation (STAMP_MESSAGE_LIST (message_list), self->account, stamp_conversation_item_get_node (item));
      adw_window_set_content (ADW_WINDOW (window), message_list);
      gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);
      gtk_window_present (GTK_WINDOW (window));
    }
    return;
  }

  state = gdk_event_get_modifier_state (event);
  ctrl = (state & GDK_CONTROL_MASK) != 0;
  shift = (state & GDK_SHIFT_MASK) != 0;

  if (ctrl && !shift) {
    /* Ctrl: einzelne Row toggeln, Anchor setzen */
    if (gtk_bitset_contains (self->selected, position))
      gtk_bitset_remove (self->selected, position);
    else
      gtk_bitset_add (self->selected, position);
    self->anchor_position = position;
  } else if (shift && !ctrl) {
    guint from;
    guint to;
    /* Shift: Bereich vom Anchor bis hier auswählen */
    gtk_bitset_remove_all (self->selected);
    from = MIN (self->anchor_position, position);
    to = MAX (self->anchor_position, position);
    gtk_bitset_add_range (self->selected, from, to - from + 1);
  } else if (ctrl && shift) {
    /* Ctrl+Shift: Bereich hinzufügen ohne bestehende zu verlieren */
    guint from = MIN (self->anchor_position, position);
    guint to = MAX (self->anchor_position, position);
    gtk_bitset_add_range (self->selected, from, to - from + 1);
  } else {
    /* Kein Modifier: nur diese Row, Anchor setzen */
    if (gtk_bitset_contains (self->selected, position)) {
      /* Nochmal klicken → abwählen */
      gtk_bitset_remove (self->selected, position);
    } else {
      /* gtk_bitset_remove_all(self->selected); */
      gtk_bitset_add (self->selected, position);
    }
    self->anchor_position = position;
  }

  /* if (gtk_bitset_contains(self->selected, position)) { */
  /*     gtk_bitset_remove(self->selected, position); */
  /*     gtk_check_button_set_active(GTK_CHECK_BUTTON(check), FALSE); */
  /* } else { */
  /*     gtk_bitset_add(self->selected, position); */
  /*     gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE); */
  /* } */

  /* Callback benachrichtigen falls gewünscht */
  /* on_selection_changed(GTK_CHECK_BUTTON (check), NULL, list_item); */
  update_selection_title (self);

  refresh_checkboxes (self);
}

static void
on_touch_begin (GtkGestureClick *gesture,
                gint             n_press,
                gdouble          x,
                gdouble          y,
                gpointer         user_data)
{
  RowData *data = g_object_get_data (G_OBJECT (gesture), "row-data");
  data->start_x = x;
  data->start_y = y;
}

static void
on_touch_released (GtkGestureClick *gesture,
                   gint             n_press,
                   gdouble          x,
                   gdouble          y,
                   gpointer         user_data)
{
  RowData *data = g_object_get_data (G_OBJECT (gesture), "row-data");
  StampConversationList *self = data->self;
  GtkListItem *list_item = data->list_item;
  StampConversationItem *item = STAMP_CONVERSATION_ITEM (gtk_list_item_get_item (list_item));

  /* Bewegung prüfen – wenn zu viel bewegt wurde war es ein Scroll */
  gdouble dx = x - data->start_x;
  gdouble dy = y - data->start_y;
  guint position;
  if (dx * dx + dy * dy > 10 * 10)
    return;

  position = gtk_list_item_get_position (list_item);

  if (self->selection_mode) {
    if (gtk_bitset_contains (self->selected, position))
      gtk_bitset_remove (self->selected, position);
    else
      gtk_bitset_add (self->selected, position);

    self->anchor_position = position;
    refresh_checkboxes (self);
    update_selection_title (self);
    return;
  }

  if (gtk_bitset_contains (self->selected, position))
    gtk_bitset_remove (self->selected, position);
  else
    gtk_bitset_add (self->selected, position);

  self->anchor_position = position;
  refresh_checkboxes (self);

  if (item) {
    g_signal_emit (self, signals[CONVERSATION_SELECTED], 0, stamp_conversation_item_get_node (item));
  } else {
    g_signal_emit (self, signals[CONVERSATION_SELECTED], 0, 0);
  }
}

static GdkContentProvider *
on_conversation_drag_prepare (GtkDragSource *source,
                              gdouble        x,
                              gdouble        y,
                              gpointer       user_data)
{
  GdkEvent *event;
  GtkListItem *list_item;
  StampConversationItem *item;

  event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (source));
  if (event) {
    GdkDevice *device;

    device = gdk_event_get_device (event);
    if (device && gdk_device_get_source (device) == GDK_SOURCE_TOUCHSCREEN)
      return NULL;
  }

  list_item = GTK_LIST_ITEM (user_data);
  item = STAMP_CONVERSATION_ITEM (gtk_list_item_get_item (list_item));

  if (!item)
    return NULL;

  g_type_ensure (STAMP_TYPE_CONVERSATION_ITEM);
  return gdk_content_provider_new_typed (STAMP_TYPE_CONVERSATION_ITEM, item);
}

static void
on_conversation_drag_begin (GtkDragSource *source,
                            GdkDrag       *drag,
                            GtkWidget     *row)
{
  GtkWidget *parent;
  GdkRGBA background_color;
  const gfloat shadow_padding = 16;
  const gfloat content_padding = 12;
  const gfloat vertical_padding = 6;
  const GdkRGBA shadow_color = { 0, 0, 0, 0.2 };
  gfloat card_width;
  gfloat content_scale;
  GskRoundedRect card;
  graphene_size_t size;
  g_autoptr (GtkSnapshot) snapshot = NULL;
  g_autoptr (GdkPaintable) paintable = NULL;
  g_autoptr (GtkWidget) color_probe = NULL;

  parent = gtk_widget_get_parent (row);
  if (!parent)
    return;

  card_width = MIN (gtk_widget_get_width (row), 360);
  if (card_width <= 2 * content_padding)
    return;

  content_scale = (card_width - 2 * content_padding) / gtk_widget_get_width (row);
  size = GRAPHENE_SIZE_INIT (card_width, gtk_widget_get_height (row) * content_scale + 2 * vertical_padding);
  gsk_rounded_rect_init_from_rect (&card, &GRAPHENE_RECT_INIT (0, 0, size.width, size.height), 12);

  snapshot = gtk_snapshot_new ();
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (shadow_padding, shadow_padding));
  gtk_snapshot_append_outset_shadow (snapshot, &card, &shadow_color, 0, 2, 0, 8);
  gtk_snapshot_push_rounded_clip (snapshot, &card);

  color_probe = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_add_css_class (color_probe, "conversation-drag-background");
  gtk_widget_get_color (color_probe, &background_color);
  gtk_snapshot_append_color (snapshot, &background_color, &card.bounds);
  gtk_snapshot_save (snapshot);

  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (content_padding, vertical_padding));
  gtk_snapshot_scale (snapshot, content_scale, content_scale);
  gtk_widget_snapshot_child (parent, row, snapshot);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);

  size.width += 2 * shadow_padding;
  size.height += 2 * shadow_padding;
  paintable = gtk_snapshot_free_to_paintable (g_steal_pointer (&snapshot), &size);

  if (paintable)
    gtk_drag_source_set_icon (source, paintable, shadow_padding + content_padding, shadow_padding + vertical_padding);
}

static void
on_conversation_drag_cancel (GtkDragSource *source,
                             GdkDrag       *drag,
                             GdkDragAction  action,
                             gpointer       user_data)
{
}

static void
on_setup_list_item (GtkListItemFactory *factory,
                    GtkListItem        *list_item,
                    gpointer            user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  GtkWidget *row = stamp_conversation_row_new ();
  GtkGesture *gesture = gtk_gesture_click_new ();
  GtkGesture *long_press = gtk_gesture_long_press_new ();
  GtkGesture *press_gesture;
  RowData *row_data;
  GtkDragSource *drag_source;

  GtkGesture *touch = gtk_gesture_click_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (touch), TRUE);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (touch));
  row_data = g_new (RowData, 1);
  row_data->self = self;
  row_data->list_item = list_item;
  g_object_set_data_full (G_OBJECT (touch), "row-data", row_data, g_free);
  g_signal_connect (touch, "pressed", G_CALLBACK (on_touch_begin), NULL);
  g_signal_connect (touch, "released", G_CALLBACK (on_touch_released), NULL);

  row_data = g_new (RowData, 1);
  row_data->self = self;
  row_data->list_item = list_item;
  press_gesture = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (press_gesture), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (press_gesture),
                                              GTK_PHASE_BUBBLE);
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (press_gesture), FALSE);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (press_gesture));
  g_object_set_data_full (G_OBJECT (press_gesture), "row-data", row_data, g_free);
  g_signal_connect (press_gesture, "pressed", G_CALLBACK (on_row_pressed), NULL);

  /* Secondary button click */
  row_data = g_new (RowData, 1);
  row_data->self = self;
  row_data->list_item = list_item;
  g_object_set_data_full (G_OBJECT (gesture), "row-data", row_data, g_free);
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), GDK_BUTTON_SECONDARY);
  g_signal_connect (gesture, "released", G_CALLBACK (on_released), NULL);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (gesture));

  /* Long press gesture */
  row_data = g_new (RowData, 1);
  row_data->self = self;
  row_data->list_item = list_item;
  g_object_set_data_full (G_OBJECT (long_press), "row-data", row_data, g_free);
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (long_press), TRUE);
  g_signal_connect (long_press, "pressed", G_CALLBACK (on_long_press_pressed), self);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (long_press));

  g_signal_connect_object (row, "mark-read", G_CALLBACK (on_row_mark_read), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (row, "mark-unread", G_CALLBACK (on_row_mark_unread), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (row, "trash", G_CALLBACK (on_row_trash), self, G_CONNECT_DEFAULT);

  drag_source = gtk_drag_source_new ();
  gtk_drag_source_set_actions (drag_source, GDK_ACTION_MOVE);
  g_signal_connect (drag_source, "prepare", G_CALLBACK (on_conversation_drag_prepare), list_item);
  g_signal_connect (drag_source, "drag-begin", G_CALLBACK (on_conversation_drag_begin), row);
  g_signal_connect (drag_source, "drag-cancel", G_CALLBACK (on_conversation_drag_cancel), row);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (drag_source));

  g_object_bind_property (list_item, "selected", row, "selected", G_BINDING_SYNC_CREATE);
  g_object_set_data (G_OBJECT (list_item), "model", self->multi_selection);
  gtk_list_item_set_child (list_item, row);
}

static void
on_check_toggled (GtkCheckButton *check,
                  gpointer        user_data)
{
  GtkListItem *list_item = GTK_LIST_ITEM (user_data);
  GtkWidget *list_view = gtk_widget_get_ancestor (GTK_WIDGET (check), GTK_TYPE_LIST_VIEW);
  GtkSelectionModel *model;
  guint position;

  if (!list_view)
    return;

  model = gtk_list_view_get_model (GTK_LIST_VIEW (list_view));
  position = gtk_list_item_get_position (list_item);

  if (gtk_check_button_get_active (check))
    gtk_selection_model_select_item (model, position, FALSE);
  else
    gtk_selection_model_unselect_item (model, position);
}

static void
on_selection_changed (GtkCheckButton *check,
                      GParamSpec     *pspec,
                      GtkListItem    *list_item)
{
  g_signal_handlers_block_by_func (check, on_check_toggled, list_item);
  gtk_check_button_set_active (check, gtk_list_item_get_selected (list_item));
  g_signal_handlers_unblock_by_func (check, on_check_toggled, list_item);
}

static void
on_bind_list_item (GtkListItemFactory *factory,
                   GtkListItem        *list_item,
                   gpointer            user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  GtkWidget *row;
  StampConversationItem *model;
  GtkCheckButton *check_button;

  row = gtk_list_item_get_child (list_item);
  model = STAMP_CONVERSATION_ITEM (gtk_list_item_get_item (list_item));

  stamp_conversation_row_bind_mail (STAMP_CONVERSATION_ROW (row), model, get_item_account (self, model), self->is_collection);
  stamp_conversation_row_set_selection_visible (STAMP_CONVERSATION_ROW (row), self->selection_mode);

  check_button = stamp_conversation_row_get_check_button (STAMP_CONVERSATION_ROW (row));
  gtk_check_button_set_active (check_button, gtk_list_item_get_selected (list_item));
  g_signal_connect (check_button, "toggled", G_CALLBACK (on_check_toggled), list_item);

  g_hash_table_add (self->bound_list_items, list_item);

  g_signal_connect_swapped (list_item, "notify::selected", G_CALLBACK (on_selection_changed), check_button);
}

static void
on_unbind_list_item (GtkListItemFactory *factory,
                     GtkListItem        *list_item,
                     gpointer            user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  GtkWidget *row;
  GtkCheckButton *check_button;
  StampConversationItem *model;
  g_hash_table_remove (self->bound_list_items, list_item);

  row = gtk_list_item_get_child (list_item);
  model = STAMP_CONVERSATION_ITEM (gtk_list_item_get_item (list_item));

  stamp_conversation_row_unbind_mail (STAMP_CONVERSATION_ROW (row), model);
  check_button = stamp_conversation_row_get_check_button (STAMP_CONVERSATION_ROW (row));
  g_signal_handlers_disconnect_by_func (check_button, on_check_toggled, list_item);
  g_signal_handlers_disconnect_by_func (list_item, on_selection_changed, check_button);
}

static void
on_single_selection_changed (GtkSelectionModel *model,
                             guint              position,
                             guint              n_items,
                             gpointer           user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationItem *conversation_item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (model)));

  if (self->selection_mode)
    return;

  if (conversation_item) {
    g_signal_emit (self, signals[CONVERSATION_SELECTED], 0, stamp_conversation_item_get_node (conversation_item));
  } else {
    g_signal_emit (self, signals[CONVERSATION_SELECTED], 0, 0);
  }
}

static void
on_select_all (GtkWidget *button,
               gpointer   user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->multi_selection));

  gtk_bitset_add_range (self->selected, 0, n_items);
  update_selection_title (self);
  refresh_checkboxes (self);
}

static void
on_unselect_all (GtkWidget *button,
                 gpointer   user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  gtk_bitset_remove_all (self->selected);
  update_selection_title (self);
  refresh_checkboxes (self);
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                gpointer               user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  GtkWidget *focus;
  gboolean shift;

  if (!self->selection_mode)
    return GDK_EVENT_PROPAGATE;

  if ((keyval != GDK_KEY_a && keyval != GDK_KEY_A) || (state & GDK_CONTROL_MASK) == 0 || (state & (GDK_ALT_MASK | GDK_META_MASK)) != 0)
    return GDK_EVENT_PROPAGATE;

  shift = (state & GDK_SHIFT_MASK) != 0;

  focus = gtk_root_get_focus (gtk_widget_get_root (GTK_WIDGET (self)));
  if (focus && gtk_widget_is_ancestor (focus, self->search_entry))
    return GDK_EVENT_PROPAGATE;

  if (shift) {
    on_unselect_all (NULL, self);
  } else {
    on_select_all (NULL, self);
  }

  return GDK_EVENT_STOP;
}

static void
on_selection_button_clicked (GtkWidget *button,
                             gpointer   user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  set_selection_active (self, !self->selection_mode);
}

static gboolean
scroll_to_top_tick (GtkWidget     *widget,
                    GdkFrameClock *clock,
                    gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));

  if (gtk_adjustment_get_value (adj) > 1.0) {
    gtk_adjustment_set_value (adj, 0.0);
    return G_SOURCE_CONTINUE;
  }

  gtk_widget_set_visible (self->scroll_to_top, FALSE);
  return G_SOURCE_REMOVE;
}

static void
on_scroll_to_top (GtkButton *button,
                  gpointer   user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  gtk_list_view_scroll_to (GTK_LIST_VIEW (self->listview), 0, GTK_LIST_SCROLL_NONE, NULL);
  gtk_widget_add_tick_callback (GTK_WIDGET (self), scroll_to_top_tick, self, NULL);
}

static void
stamp_conversation_list_get_property (GObject    *object,
                                      guint       property_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (object);

  switch ((StampConversationListProps)property_id) {
    case PROP_STATE:
      g_value_set_boolean (value, self->selection_mode);
      break;
  }
}

static void
stamp_conversation_list_set_property (GObject      *object,
                                      guint         property_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  switch ((StampConversationListProps)property_id) {
    case PROP_STATE:
      break;
  }
}

static void
stamp_conversation_list_dispose (GObject *object)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_cancellable_cancel (self->thread_cancellable);
  g_clear_object (&self->thread_cancellable);

  g_cancellable_cancel (self->transfer_cancellable);
  g_clear_object (&self->transfer_cancellable);

  g_clear_handle_id (&self->load_folder_handler, g_source_remove);

  g_clear_object (&self->thread);
  g_clear_object (&self->list_store);
  g_clear_pointer (&self->full_name, g_free);
  g_clear_pointer (&self->thread_cache, g_hash_table_unref);
  g_clear_pointer (&self->folders, g_hash_table_unref);
  g_clear_list (&self->moved_messages, g_object_unref);
  g_clear_object (&self->account);
  g_clear_object (&self->folder);
  if (self->collection)
    g_signal_handlers_disconnect_by_func (self->collection, on_collection_sources_changed, self);
  g_clear_object (&self->collection);

  for (GList *iter = self->collection_folders; iter; iter = iter->next)
    g_signal_handlers_disconnect_by_func (iter->data, on_collection_folder_changed, self);

  g_list_free_full (g_steal_pointer (&self->collection_folders), g_object_unref);

  g_clear_pointer (&self->selected, gtk_bitset_unref);
  g_clear_pointer (&self->trash_array, g_ptr_array_unref);
  g_clear_list (&self->trash_entries, trash_entry_free);

  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_CONVERSATION_LIST);
  g_clear_pointer (&self->bound_list_items, g_hash_table_unref);

  G_OBJECT_CLASS (stamp_conversation_list_parent_class)->dispose (object);
}

static gboolean
on_scroll (GtkEventControllerScroll *controller,
           gdouble                   dx,
           gdouble                   dy,
           gpointer                  user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));
  gdouble adjustment = gtk_adjustment_get_value (adj);

  if (adjustment > 200 && dy < 0) {
    gtk_widget_set_visible (self->scroll_to_top, TRUE);
  } else {
    gtk_widget_set_visible (self->scroll_to_top, FALSE);
  }

  return FALSE;
}

static void
stamp_conversation_list_class_init (StampConversationListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/conversation-list/stamp-conversation-list.ui");

  object_class->get_property = stamp_conversation_list_get_property;
  object_class->set_property = stamp_conversation_list_set_property;
  object_class->dispose = stamp_conversation_list_dispose;

  gtk_widget_class_bind_template_child (widget_class, StampConversationList, listview);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, conversation_view);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, search_bar);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, search_entry);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, window_title);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, new_message);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, selection_bottom_bar);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, scroll_to_top);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, scrolled_window);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, header_stack);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, normal_headerbar);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, selection_headerbar);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, selection_label);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, spinner);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, sort_or_filter_button);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, mail_list_stack);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, sidebar_button);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, sort_is_active);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, context_menu_model);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, move_selection_button);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, trash_selection_button);
  gtk_widget_class_bind_template_child (widget_class, StampConversationList, mark_read_selection_button);

  gtk_widget_class_bind_template_callback (widget_class, on_mail_search_entry_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_new_message);
  gtk_widget_class_bind_template_callback (widget_class, on_setup_list_item);
  gtk_widget_class_bind_template_callback (widget_class, on_bind_list_item);
  gtk_widget_class_bind_template_callback (widget_class, on_unbind_list_item);
  gtk_widget_class_bind_template_callback (widget_class, on_selection_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_scroll_to_top);
  gtk_widget_class_bind_template_callback (widget_class, on_scroll);
  gtk_widget_class_bind_template_callback (widget_class, on_select_all);
  gtk_widget_class_bind_template_callback (widget_class, on_unselect_all);
  gtk_widget_class_bind_template_callback (widget_class, on_key_pressed);

  signals[CONVERSATION_SELECTED] = g_signal_new ("conversation-selected", G_OBJECT_CLASS_TYPE (klass),
                                                 G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                                 0, NULL, NULL, NULL,
                                                 G_TYPE_NONE,
                                                 1, G_TYPE_POINTER);
  signals[CONVERSATION_TRASH] = g_signal_new ("conversation-trash", G_OBJECT_CLASS_TYPE (klass),
                                              G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                              0, NULL, NULL, NULL,
                                              G_TYPE_NONE,
                                              1, STAMP_TYPE_CONVERSATION_ITEM);

  properties[PROP_STATE] = g_param_spec_boolean ("state",
                                                 NULL,
                                                 NULL,
                                                 FALSE,
                                                 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);
}

static void
on_multi_selection_changed (GtkSelectionModel *model,
                            guint              pos,
                            guint              n_items,
                            gpointer           user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  guint n_selected = gtk_bitset_get_size (self->selected);
  gchar buf[64];

  g_snprintf (buf, sizeof buf, _("%u selected"), n_selected);

  adw_window_title_set_title (ADW_WINDOW_TITLE (self->selection_label), buf);
}

static void
on_drag_update (GtkGesturePan   *gesture,
                GtkPanDirection  direction,
                gdouble          offset,
                gpointer         user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  gdouble clean_y = offset;
  GValue value = G_VALUE_INIT;
  g_value_init (&value, G_TYPE_INT);

  if (!self->claimed) {
    if (direction != GTK_PAN_DIRECTION_DOWN) {
      gtk_widget_set_visible (self->scroll_to_top, FALSE);
      return;
    }

    if (gtk_adjustment_get_value (self->vadj) > 0.0) {
      gtk_widget_set_visible (self->scroll_to_top, TRUE);
      return;
    }
    gtk_widget_set_visible (self->scroll_to_top, FALSE);

    if (offset < 10)
      return;

    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    gtk_widget_set_sensitive (GTK_WIDGET (self->scrolled_window), FALSE);
    self->claimed = TRUE;
  }

  gtk_widget_set_visible (self->spinner, TRUE);


  if (offset > 150)
    clean_y = 150;

  if (clean_y < 0)
    clean_y = 0;

  if (clean_y > 32) {
    gtk_widget_set_margin_top (GTK_WIDGET (self->spinner), clean_y);
    g_value_set_int (&value, 32);
    gtk_widget_set_size_request (GTK_WIDGET (self->spinner), 32, 32);
  } else if (clean_y > 0) {
    g_value_set_int (&value, clean_y);
    gtk_widget_set_size_request (GTK_WIDGET (self->spinner), clean_y, clean_y);
  } else {
    gtk_widget_set_margin_top (GTK_WIDGET (self->spinner), 32);
    g_value_set_int (&value, 0);
    gtk_widget_set_size_request (GTK_WIDGET (self->spinner), 0, 0);
  }

  self->is_pulling = TRUE;
}

static void
update_folder_idle (gpointer user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  self->load_folder_handler = 0;
  rebuild_list (self);
}

static void
load_folder_idle (gpointer user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  stamp_conversation_list_refresh (self);

  self->load_folder_handler = 0;
}

static void
on_drag_end (GtkGestureDrag *gesture,
             gdouble         dx,
             gdouble         dy,
             gpointer        user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  if (gtk_widget_get_margin_top (GTK_WIDGET (self->spinner)) >= 125) {
    g_clear_handle_id (&self->load_folder_handler, g_source_remove);
    self->load_folder_handler = g_idle_add_once (load_folder_idle, self);
  } else {
    gtk_widget_set_visible (self->spinner, FALSE);
    gtk_widget_set_margin_top (self->spinner, 12);
  }

  gtk_widget_set_sensitive (GTK_WIDGET (self->scrolled_window), TRUE);
  self->claimed = FALSE;
}

static void
on_filter_activate (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  const gchar *value;

  if (!parameter)
    return;

  /* neuen Zustand setzen */
  g_simple_action_set_state (action, parameter);

  value = g_variant_get_string (parameter, NULL);

  if (g_strcmp0 (value, "all-mails") == 0)
    self->filter_mode = FILTER_MODE_ALL;
  else if (g_strcmp0 (value, "starred") == 0)
    self->filter_mode = FILTER_MODE_STARRED;
  else if (g_strcmp0 (value, "attachment") == 0)
    self->filter_mode = FILTER_MODE_ATTACHMENT;
  else if (g_strcmp0 (value, "unread") == 0)
    self->filter_mode = FILTER_MODE_UNREAD;

  gtk_widget_set_visible (GTK_WIDGET (self->sort_is_active), g_strcmp0 (value, "all-mails") != 0);

  if (self->filter)
    gtk_filter_changed (self->filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static void
on_sort_activate (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  const gchar *value;

  if (!parameter)
    return;

  /* neuen Zustand setzen */
  g_simple_action_set_state (action, parameter);

  value = g_variant_get_string (parameter, NULL);

  if (g_strcmp0 (value, "newest-first") == 0)
    self->sort_mode = SORT_MODE_NEWEST_FIRST;
  else
    self->sort_mode = SORT_MODE_OLDEST_FIRST;

  gtk_sorter_changed (self->sorter, GTK_SORTER_CHANGE_INVERTED);
}

static void
on_mark_category (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationItem *item = NULL;
  const gchar *value;

  if (!parameter)
    return;

  value = g_variant_get_string (parameter, NULL);

  item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));

  stamp_conversation_item_set_label (item, value, TRUE);
}

static void
update_actions (StampConversationList *self)
{
  /* GAction *action; */
  /* guint32 flags = 0; */
  /* guint32 flags = camel_message_info_get_flags (self->message_info); */
  /* gboolean flagged = (flags & CAMEL_MESSAGE_FLAGGED) != 0; */
  /* gboolean read = (flags & CAMEL_MESSAGE_SEEN) != 0; */

  /* return; */

  /* action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "mark-unflag"); */
  /* g_simple_action_set_enabled (G_SIMPLE_ACTION (action), flagged); */
  /* action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "mark-flag"); */
  /* g_simple_action_set_enabled (G_SIMPLE_ACTION (action), !flagged); */

  /* action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "mark-unread"); */
  /* g_simple_action_set_enabled (G_SIMPLE_ACTION (action), read); */
  /* action = g_action_map_lookup_action (G_ACTION_MAP (self->actions), "mark-read"); */
  /* g_simple_action_set_enabled (G_SIMPLE_ACTION (action), !read); */
}

static void
on_mark_read (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  stamp_conversation_list_mark_read (self, NULL);
}

static void
on_mark_unread (GSimpleAction *action,
                GVariant      *parameter,
                gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  stamp_conversation_list_mark_unread (self, NULL);
}

static void
on_mark_starred (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationItem *item = NULL;

  item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));

  stamp_conversation_item_set_flagged (item, TRUE);
  update_actions (self);
}

static void
on_mark_unstarred (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  StampConversationItem *item = NULL;

  item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));

  stamp_conversation_item_set_flagged (item, FALSE);
  update_actions (self);
}

static GPtrArray *
collect_messages (CamelFolderThreadNode *node,
                  GPtrArray             *array)
{
  CamelFolderThreadNode *child = camel_folder_thread_node_get_child (node);

  if (!array) {
    array = g_ptr_array_new ();
  }

  g_ptr_array_add (array, node);

  while (child) {
    array = collect_messages (child, array);
    child = camel_folder_thread_node_get_next (child);
  }

  return array;
}

static GPtrArray *
collect_uids_for_folder (CamelFolderThreadNode *node,
                         CamelFolder           *folder)
{
  GPtrArray *uids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) all = collect_messages (node, NULL);

  for (guint i = 0; i < all->len; i++) {
    CamelFolderThreadNode *child_node = all->pdata[i];
    const CamelMessageInfo *info = camel_folder_thread_node_get_item (child_node);
    g_autoptr (CamelFolderSummary) summary = NULL;
    CamelFolder *msg_folder;

    g_object_get (G_OBJECT (info), "summary", &summary, NULL);
    msg_folder = camel_folder_summary_get_folder (summary);

    if (msg_folder == folder)
      g_ptr_array_add (uids, g_strdup (camel_message_info_get_uid (info)));
  }

  return uids;
}

static void
on_transfer_messages_to (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  CamelFolder *folder = CAMEL_FOLDER (source);
  g_autoptr (GError) error = NULL;

  if (!camel_folder_transfer_messages_to_finish (folder, res, NULL, &error)) {
    g_warning ("%s: Could not move message: %s", G_STRFUNC, error->message);
    return;
  }

  camel_folder_synchronize_sync (folder, FALSE, NULL, &error);
  if (error) {
    g_warning ("%s: Could not synchronize folder: %s", G_STRFUNC, error->message);
  }
}

void
stamp_conversation_list_move_conversation (StampConversationList *self,
                                           StampConversationItem *item,
                                           const gchar           *target_full_name)
{
  StampMailService *mail_service;
  CamelStore *store;
  CamelFolder *target;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) uid_array = NULL;

  g_return_if_fail (STAMP_IS_CONVERSATION_LIST (self));
  g_return_if_fail (STAMP_IS_CONVERSATION_ITEM (item));

  stamp_conversation_item_set_hidden (item, TRUE);

  uid_array = collect_uids_for_folder (stamp_conversation_item_get_node (item), self->folder);

  g_list_model_items_changed (G_LIST_MODEL (self->list_store),
                              0,
                              g_list_model_get_n_items (G_LIST_MODEL (self->list_store)),
                              g_list_model_get_n_items (G_LIST_MODEL (self->list_store)));

  if (uid_array->len == 0) {
    g_warning ("%s: No uids from current folder, abort", G_STRFUNC);
    return;
  }

  mail_service = stamp_account_get_mail_service (self->account);
  store = CAMEL_STORE (stamp_mail_service_get_service (mail_service));
  target = camel_store_get_folder_sync (store, target_full_name, 0, NULL, &error);
  if (error) {
    g_warning ("%s: Could not open target folder %s: %s", G_STRFUNC, target_full_name, error->message);
    return;
  }

  if (self->transfer_cancellable) {
    g_cancellable_cancel (self->transfer_cancellable);
    g_clear_object (&self->transfer_cancellable);
  }
  self->transfer_cancellable = g_cancellable_new ();

  camel_folder_transfer_messages_to (self->folder, uid_array, target, TRUE,
                                     G_PRIORITY_DEFAULT, self->transfer_cancellable,
                                     on_transfer_messages_to, self);

  g_object_unref (target);
}

static void
on_move_folder (GSimpleAction *action,
                GVariant      *parameter,
                gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  const gchar *target_full_name = g_variant_get_string (parameter, NULL);
  StampMailService *mail_service;
  CamelStore *store;
  CamelFolder *target;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) node_array = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GPtrArray) uid_array = NULL;

  if (self->selection_mode) {
    GtkBitset *selected_items = self->selected;
    GtkBitsetIter iter;
    guint32 pos;

    gtk_bitset_iter_init_first (&iter, selected_items, &pos);
    while (gtk_bitset_iter_is_valid (&iter)) {
      g_autoptr (StampConversationItem) item = STAMP_CONVERSATION_ITEM (
        g_list_model_get_item (G_LIST_MODEL (self->multi_selection), pos));

      g_ptr_array_add (node_array, g_object_ref (item));
      gtk_bitset_iter_next (&iter, &pos);
    }
  }

  uid_array = g_ptr_array_new_with_free_func (g_free);

  for (gint idx = 0; idx < node_array->len; idx++) {
    StampConversationItem *item = g_ptr_array_index (node_array, idx);
    g_autoptr (GPtrArray) folder_uids = NULL;

    stamp_conversation_item_set_hidden (item, TRUE);
    folder_uids = collect_uids_for_folder (stamp_conversation_item_get_node (item), self->folder);

    for (guint ui = 0; ui < folder_uids->len; ui++)
      g_ptr_array_add (uid_array, g_strdup (g_ptr_array_index (folder_uids, ui)));
  }

  g_list_model_items_changed (G_LIST_MODEL (self->list_store),
                              0,
                              g_list_model_get_n_items (G_LIST_MODEL (self->list_store)),
                              g_list_model_get_n_items (G_LIST_MODEL (self->list_store)));

  if (uid_array->len == 0) {
    g_warning ("%s: No uids from current folder, abort", G_STRFUNC);
    return;
  }

  mail_service = stamp_account_get_mail_service (self->account);
  store = CAMEL_STORE (stamp_mail_service_get_service (mail_service));
  target = camel_store_get_folder_sync (store, target_full_name, 0, NULL, &error);
  if (error) {
    g_warning ("Could not open target folder %s: %s", target_full_name, error->message);
    return;
  }

  if (self->transfer_cancellable) {
    g_cancellable_cancel (self->transfer_cancellable);
    g_clear_object (&self->transfer_cancellable);
  }
  self->transfer_cancellable = g_cancellable_new ();

  camel_folder_transfer_messages_to (self->folder, uid_array, target, TRUE,
                                     G_PRIORITY_DEFAULT, self->transfer_cancellable,
                                     on_transfer_messages_to, self);

  gtk_bitset_remove_all (self->selected);
  update_selection_title (self);
  refresh_checkboxes (self);
  set_selection_active (self, FALSE);

  g_object_unref (target);
}

static void
on_trash_folder (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);
  g_autoptr (GPtrArray) uid_array = NULL;
  guint n_items;

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->list_store));
  if (n_items == 0)
    return;

  uid_array = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < n_items; i++) {
    StampConversationItem *item = STAMP_CONVERSATION_ITEM (g_list_model_get_item (G_LIST_MODEL (self->list_store), i));
    CamelFolderThreadNode *node = stamp_conversation_item_get_node (item);
    g_autoptr (GPtrArray) folder_uids = collect_uids_for_folder (node, self->folder);

    for (guint ui = 0; ui < folder_uids->len; ui++)
      g_ptr_array_add (uid_array, g_strdup (g_ptr_array_index (folder_uids, ui)));
  }

  if (uid_array->len > 0) {
    g_autoptr (GError) error = NULL;

    for (guint i = 0; i < uid_array->len; i++)
      camel_folder_set_message_flags (self->folder, uid_array->pdata[i], CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_DELETED);

    camel_folder_expunge_sync (self->folder, NULL, &error);
    if (error) {
      g_warning ("%s: Could not expunge folder: %s", G_STRFUNC, error->message);
    }
  }
}

static void
on_trash (GSimpleAction *action,
          GVariant      *parameter,
          gpointer       user_data)
{
  StampConversationList *self = STAMP_CONVERSATION_LIST (user_data);

  stamp_conversation_list_trash (self, NULL);
}

static const GActionEntry stamp_conversation_list_action_entries[] = {
  { .name = "mark-category", .activate = on_mark_category, .parameter_type = "s" },
  { .name = "mark-read", .activate = on_mark_read },
  { .name = "mark-unread", .activate = on_mark_unread },
  { .name = "mark-starred", .activate = on_mark_starred },
  { .name = "mark-unstarred", .activate = on_mark_unstarred },
  { .name = "move-folder", .activate = on_move_folder, .parameter_type = "s" },
  { .name = "trash-folder", .activate = on_trash_folder },
  { .name = "trash", .activate = on_trash },
};

static void
stamp_conversation_list_init (StampConversationList *self)
{
  GtkSortListModel *sort_model;
  GtkGesture *drag;
  GSimpleActionGroup *actions = g_simple_action_group_new ();
  GtkApplication *app = GTK_APPLICATION (g_application_get_default ());
  GMenu *menu;
  GMenu *filter_menu;
  GMenu *sort_menu;
  GSimpleAction *all_mails_action = g_simple_action_new_stateful ("mail-filter", G_VARIANT_TYPE_STRING, g_variant_new_string ("all-mails"));
  GSimpleAction *sort_action = g_simple_action_new_stateful ("mail-sort", G_VARIANT_TYPE_STRING, g_variant_new_string ("newest-first"));

  gtk_widget_init_template (GTK_WIDGET (self));

  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions),
                                   stamp_conversation_list_action_entries,
                                   G_N_ELEMENTS (stamp_conversation_list_action_entries),
                                   self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "conversation-list", G_ACTION_GROUP (self->actions));

  self->thread_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, cached_thread_free);

  self->thread_cancellable = g_cancellable_new ();

  g_signal_connect_object (all_mails_action, "activate", G_CALLBACK (on_filter_activate), self, G_CONNECT_DEFAULT);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (all_mails_action));
  g_signal_connect_object (sort_action, "activate", G_CALLBACK (on_sort_activate), self, G_CONNECT_DEFAULT);
  g_action_map_add_action (G_ACTION_MAP (actions), G_ACTION (sort_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "conversation", G_ACTION_GROUP (actions));

  menu = g_menu_new ();
  filter_menu = g_menu_new ();
  g_menu_append (filter_menu, _("All Mails"), "app.mail-filter::all-mails");
  g_menu_append (filter_menu, _("Starred"), "app.mail-filter::starred");
  g_menu_append (filter_menu, _("Attachment"), "app.mail-filter::attachment");
  g_menu_append (filter_menu, _("Unread"), "app.mail-filter::unread");
  g_menu_append_section (menu, _("Filter"), G_MENU_MODEL (filter_menu));

  sort_menu = g_menu_new ();
  g_menu_append_section (menu, _("Sort Order"), G_MENU_MODEL (sort_menu));
  g_menu_append (sort_menu, _("Newest First"), "conversation.mail-sort::newest-first");
  g_menu_append (sort_menu, _("Oldest First"), "conversation.mail-sort::oldest-first");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (self->sort_or_filter_button), G_MENU_MODEL (menu));

  self->is_pulling = FALSE;

  self->folders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  self->list_store = g_list_store_new (STAMP_TYPE_CONVERSATION_ITEM);

  self->filter = GTK_FILTER (gtk_custom_filter_new (filter_func, self, NULL));
  self->filter_model = gtk_filter_list_model_new (G_LIST_MODEL (self->list_store), self->filter);
  /* self->filter_model = gtk_filter_list_model_new (NULL, self->filter); */

  self->sorter = GTK_SORTER (gtk_custom_sorter_new (sorter_func, self, NULL));
  sort_model = gtk_sort_list_model_new (G_LIST_MODEL (self->filter_model), self->sorter);

  self->important_first = g_settings_get_boolean (STAMP_SETTINGS_MAIL, STAMP_PREFS_MAIL_IMPORTANT_FIRST);
  g_signal_connect_object (STAMP_SETTINGS_MAIL, "changed::" STAMP_PREFS_MAIL_IMPORTANT_FIRST, G_CALLBACK (on_important_first_changed), self, G_CONNECT_DEFAULT);

  g_signal_connect (G_LIST_MODEL (sort_model), "items-changed", G_CALLBACK (on_items_changed), self);

  rebuild_category_actions (self);

  self->single_selection = gtk_single_selection_new (G_LIST_MODEL (sort_model));
  g_signal_connect_object (self->single_selection, "selection-changed", G_CALLBACK (on_single_selection_changed), self, G_CONNECT_DEFAULT);
  gtk_list_view_set_model (GTK_LIST_VIEW (self->listview), GTK_SELECTION_MODEL (self->single_selection));

  gtk_single_selection_set_autoselect (self->single_selection, FALSE);
  gtk_single_selection_set_can_unselect (self->single_selection, TRUE);

  self->multi_selection = gtk_no_selection_new (G_LIST_MODEL (sort_model));
  g_signal_connect_object (self->multi_selection, "selection-changed", G_CALLBACK (on_multi_selection_changed), self, G_CONNECT_DEFAULT);

  gtk_search_bar_connect_entry (GTK_SEARCH_BAR (self->search_bar), GTK_EDITABLE (self->search_entry));

  drag = gtk_gesture_pan_new (GTK_ORIENTATION_VERTICAL);
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (drag), TRUE);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag), GTK_PHASE_CAPTURE);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  self->vadj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));

  g_signal_connect_object (drag, "pan", G_CALLBACK (on_drag_update), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (drag, "drag-end", G_CALLBACK (on_drag_end), self, G_CONNECT_DEFAULT);

  self->selected = gtk_bitset_new_empty ();
  self->anchor_position = 0;
  self->bound_list_items = g_hash_table_new (g_direct_hash, g_direct_equal);
}

GtkWidget *
stamp_conversation_list_new (void)
{
  return g_object_new (STAMP_TYPE_CONVERSATION_LIST, NULL);
}

void
stamp_conversation_list_mark_read (StampConversationList *self,
                                   CamelFolderThreadNode *node)
{
  g_autoptr (GPtrArray) node_array = g_ptr_array_new ();
  g_autoptr (GPtrArray) array = NULL;

  if (self->selection_mode) {
    GtkBitset *selected_items = self->selected;
    GtkBitsetIter iter;
    guint32 current_item_position;

    gtk_bitset_iter_init_first (&iter, selected_items, &current_item_position);

    while (gtk_bitset_iter_is_valid (&iter)) {
      g_autoptr (StampConversationItem) item = STAMP_CONVERSATION_ITEM (g_list_model_get_item (G_LIST_MODEL (self->multi_selection), current_item_position));

      node = stamp_conversation_item_get_node (item);
      g_ptr_array_add (node_array, node);
      gtk_bitset_iter_next (&iter, &current_item_position);
    }

    gtk_bitset_remove_all (self->selected);
    update_selection_title (self);
    refresh_checkboxes (self);
  } else {
    if (!node) {
      StampConversationItem *item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));
      node = stamp_conversation_item_get_node (item);
    }
    g_ptr_array_add (node_array, node);
  }

  for (gint idx = 0; idx < node_array->len; idx++) {
    CamelFolderThreadNode *child_node = node_array->pdata[idx];

    array = collect_messages (child_node, array);
  }

  if (!array) {
    g_warning ("%s: No array, abort", G_STRFUNC);
    return;
  }

  for (gint idx = array->len - 1; idx >= 0; idx--) {
    CamelFolderThreadNode *child_node = array->pdata[idx];
    camel_message_info_set_flags (CAMEL_MESSAGE_INFO (camel_folder_thread_node_get_item (child_node)), CAMEL_MESSAGE_SEEN, ~0);
  }
}

void
stamp_conversation_list_mark_unread (StampConversationList *self,
                                     CamelFolderThreadNode *node)
{
  g_autoptr (GPtrArray) node_array = g_ptr_array_new ();
  g_autoptr (GPtrArray) array = NULL;

  if (self->selection_mode) {
    GtkBitset *selected_items = self->selected;
    GtkBitsetIter iter;
    guint32 current_item_position;

    gtk_bitset_iter_init_first (&iter, selected_items, &current_item_position);

    while (gtk_bitset_iter_is_valid (&iter)) {
      g_autoptr (StampConversationItem) item = STAMP_CONVERSATION_ITEM (g_list_model_get_item (G_LIST_MODEL (self->multi_selection), current_item_position));

      node = stamp_conversation_item_get_node (item);
      g_ptr_array_add (node_array, node);
      gtk_bitset_iter_next (&iter, &current_item_position);
    }

    gtk_bitset_remove_all (self->selected);
    update_selection_title (self);
    refresh_checkboxes (self);
  } else {
    if (!node) {
      StampConversationItem *item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));
      node = stamp_conversation_item_get_node (item);
    }
    g_ptr_array_add (node_array, node);
  }

  for (gint idx = 0; idx < node_array->len; idx++) {
    CamelFolderThreadNode *child_node = node_array->pdata[idx];

    array = collect_messages (child_node, array);
  }

  if (!array) {
    g_warning ("%s: No array, abort", G_STRFUNC);
    return;
  }

  for (gint idx = array->len - 1; idx >= 0; idx--) {
    CamelFolderThreadNode *child_node = array->pdata[idx];
    camel_message_info_set_flags (CAMEL_MESSAGE_INFO (camel_folder_thread_node_get_item (child_node)), CAMEL_MESSAGE_SEEN, 0);
  }
}

void
stamp_conversation_list_mark_unflag_selected_messages (StampConversationList *self)
{
  StampConversationItem *item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));
  CamelFolderThreadNode *node;

  node = stamp_conversation_item_get_node (item);
  camel_message_info_set_flags (CAMEL_MESSAGE_INFO (camel_folder_thread_node_get_item (node)), CAMEL_MESSAGE_FLAGGED, 0);
}

void
stamp_conversation_list_mark_flag_selected_messages (StampConversationList *self)
{
  StampConversationItem *item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));
  CamelFolderThreadNode *node;

  node = stamp_conversation_item_get_node (item);
  camel_message_info_set_flags (CAMEL_MESSAGE_INFO (camel_folder_thread_node_get_item (node)), CAMEL_MESSAGE_FLAGGED, ~0);
}

void
stamp_conversation_list_unselect (StampConversationList *self)
{
  gtk_selection_model_unselect_all (GTK_SELECTION_MODEL (self->single_selection));
  gtk_bitset_remove_all (self->selected);
}

StampAccount *
stamp_conversation_list_get_account (StampConversationList *self)
{
  g_return_val_if_fail (STAMP_IS_CONVERSATION_LIST (self), NULL);

  return self->account;
}

gboolean
stamp_conversation_list_is_collection (StampConversationList *self)
{
  g_return_val_if_fail (STAMP_IS_CONVERSATION_LIST (self), FALSE);

  return self->is_collection;
}

void
stamp_conversation_list_clear (StampConversationList *self)
{
  g_return_if_fail (STAMP_IS_CONVERSATION_LIST (self));

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  if (self->transfer_cancellable) {
    g_cancellable_cancel (self->transfer_cancellable);
    g_clear_object (&self->transfer_cancellable);
  }

  g_clear_handle_id (&self->load_folder_handler, g_source_remove);

  if (self->folder) {
    g_signal_handlers_disconnect_by_func (self->folder, on_conversation_list_folder_changed, self);
    g_clear_object (&self->folder);
  }

  for (GList *iter = self->collection_folders; iter; iter = iter->next)
    g_signal_handlers_disconnect_by_func (iter->data, on_collection_folder_changed, self);

  g_list_free_full (g_steal_pointer (&self->collection_folders), g_object_unref);

  if (self->collection)
    g_signal_handlers_disconnect_by_func (self->collection, on_collection_sources_changed, self);

  g_clear_object (&self->account);
  g_clear_object (&self->collection);
  g_clear_pointer (&self->full_name, g_free);

  g_list_store_splice (self->list_store,
                       0,
                       g_list_model_get_n_items (G_LIST_MODEL (self->list_store)),
                       NULL,
                       0);

  self->is_collection = FALSE;
  gtk_widget_set_visible (self->spinner, FALSE);
}

typedef struct {
  CamelFolder *source_folder;
  StampAccount *account;
  GPtrArray *uids;
} StampTrashEntry;

static void
trash_entry_free (gpointer data)
{
  StampTrashEntry *entry = data;

  g_clear_object (&entry->source_folder);
  g_clear_object (&entry->account);
  g_clear_pointer (&entry->uids, g_ptr_array_unref);
  g_free (entry);
}

static GList *
collect_trash_entries (StampConversationList *self,
                       GPtrArray             *items)
{
  g_autoptr (GHashTable) by_folder = g_hash_table_new (g_direct_hash, g_direct_equal);
  GList *entries = NULL;

  for (guint i = 0; i < items->len; i++) {
    StampConversationItem *child_item = STAMP_CONVERSATION_ITEM (items->pdata[i]);
    StampAccount *account = get_item_account (self, child_item);
    g_autoptr (GPtrArray) all = collect_messages (stamp_conversation_item_get_node (child_item), NULL);

    if (!account)
      continue;

    for (guint m = 0; m < all->len; m++) {
      CamelFolderThreadNode *child_node = all->pdata[m];
      const CamelMessageInfo *info = camel_folder_thread_node_get_item (child_node);
      g_autoptr (CamelFolderSummary) summary = NULL;
      CamelFolder *msg_folder;
      StampTrashEntry *entry;

      if (!info)
        continue;

      g_object_get (G_OBJECT (info), "summary", &summary, NULL);
      msg_folder = camel_folder_summary_get_folder (summary);
      if (!msg_folder)
        continue;

      entry = g_hash_table_lookup (by_folder, msg_folder);
      if (!entry) {
        entry = g_new0 (StampTrashEntry, 1);
        entry->source_folder = g_object_ref (msg_folder);
        entry->account = g_object_ref (account);
        entry->uids = g_ptr_array_new_with_free_func (g_free);
        g_hash_table_insert (by_folder, msg_folder, entry);
        entries = g_list_prepend (entries, entry);
      }

      g_ptr_array_add (entry->uids, g_strdup (camel_message_info_get_uid (info)));
    }
  }

  return entries;
}

static void
set_item_hidden (StampConversationList *self,
                 StampConversationItem *item,
                 gboolean               hidden)
{
  guint position;

  if (stamp_conversation_item_get_hidden (item) == hidden)
    return;

  stamp_conversation_item_set_hidden (item, hidden);
  if (g_list_store_find (self->list_store, item, &position))
    g_list_model_items_changed (G_LIST_MODEL (self->list_store), position, 1, 1);
}

gboolean
stamp_conversation_list_trash (StampConversationList *self,
                               StampConversationItem *item)
{
  g_autoptr (GPtrArray) node_array = g_ptr_array_new_with_free_func (g_object_unref);
  CamelFolder *folder;
  CamelFolder *trash_folder;
  gboolean has_uid = FALSE;

  if (self->selection_mode) {
    GtkBitset *selected_items = self->selected;
    GtkBitsetIter iter;
    guint32 current_item_position;

    gtk_bitset_iter_init_first (&iter, selected_items, &current_item_position);

    while (gtk_bitset_iter_is_valid (&iter)) {
      g_autoptr (StampConversationItem) child_item = STAMP_CONVERSATION_ITEM (g_list_model_get_item (G_LIST_MODEL (self->multi_selection), current_item_position));

      g_ptr_array_add (node_array, g_object_ref (child_item));
      gtk_bitset_iter_next (&iter, &current_item_position);
    }

    gtk_bitset_remove_all (self->selected);
    update_selection_title (self);
    refresh_checkboxes (self);
  } else {
    if (!item)
      item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));

    if (!item)
      return FALSE;

    g_ptr_array_add (node_array, g_object_ref (item));
  }

  if (!self->is_collection) {
    folder = self->folder;
    trash_folder = stamp_account_get_mail_trash_folder (self->account);

    if (folder == trash_folder || folder == stamp_account_get_mail_junk_folder (self->account)) {
      g_autoptr (GError) error = NULL;

      g_autoptr (GPtrArray) flush_uids = g_ptr_array_new_with_free_func (g_free);

      for (gint idx = 0; idx < node_array->len; idx++) {
        CamelFolderThreadNode *child_node = stamp_conversation_item_get_node (STAMP_CONVERSATION_ITEM (node_array->pdata[idx]));
        g_autoptr (GPtrArray) folder_uids = collect_uids_for_folder (child_node, folder);

        for (guint ui = 0; ui < folder_uids->len; ui++)
          g_ptr_array_add (flush_uids, g_strdup (g_ptr_array_index (folder_uids, ui)));
      }

      if (self->transfer_cancellable) {
        g_cancellable_cancel (self->transfer_cancellable);
        g_clear_object (&self->transfer_cancellable);
      }

      if (flush_uids->len == 0) {
        g_warning ("%s: No uids from current folder, abort", G_STRFUNC);
        return FALSE;
      }

      for (gint idx = flush_uids->len - 1; idx >= 0; idx--)
        camel_folder_set_message_flags (folder, flush_uids->pdata[idx], CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_DELETED, CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_DELETED);

      camel_folder_synchronize_sync (folder, FALSE, NULL, &error);
      if (error)
        g_warning ("%s: Could not synchronize folder: %s", G_STRFUNC, error->message);

      for (gint idx = 0; idx < node_array->len; idx++)
        set_item_hidden (self, STAMP_CONVERSATION_ITEM (node_array->pdata[idx]), TRUE);

      stamp_conversation_list_unselect (self);
      return FALSE;
    }
  }

  if (self->transfer_cancellable) {
    g_cancellable_cancel (self->transfer_cancellable);
    g_clear_object (&self->transfer_cancellable);
  }

  g_clear_pointer (&self->trash_array, g_ptr_array_unref);
  g_clear_list (&self->trash_entries, trash_entry_free);

  self->trash_array = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < node_array->len; i++)
    g_ptr_array_add (self->trash_array, g_object_ref (node_array->pdata[i]));

  if (self->is_collection) {
    self->trash_entries = collect_trash_entries (self, node_array);
  } else {
    StampTrashEntry *entry = g_new0 (StampTrashEntry, 1);

    entry->source_folder = g_object_ref (folder);
    entry->account = g_object_ref (self->account);
    entry->uids = g_ptr_array_new_with_free_func (g_free);

    for (gint idx = 0; idx < node_array->len; idx++) {
      CamelFolderThreadNode *child_node = stamp_conversation_item_get_node (STAMP_CONVERSATION_ITEM (node_array->pdata[idx]));
      g_autoptr (GPtrArray) folder_uids = collect_uids_for_folder (child_node, folder);

      for (guint ui = 0; ui < folder_uids->len; ui++)
        g_ptr_array_add (entry->uids, g_strdup (g_ptr_array_index (folder_uids, ui)));
    }

    self->trash_entries = g_list_append (NULL, entry);
  }

  for (GList *l = self->trash_entries; l; l = l->next) {
    StampTrashEntry *entry = l->data;

    if (entry->uids->len > 0) {
      has_uid = TRUE;
      break;
    }
  }

  if (!has_uid) {
    g_warning ("%s: No uids from current folder, abort", G_STRFUNC);
    g_clear_pointer (&self->trash_array, g_ptr_array_unref);
    g_clear_list (&self->trash_entries, trash_entry_free);
    return FALSE;
  }

  for (gint idx = 0; idx < node_array->len; idx++)
    set_item_hidden (self, STAMP_CONVERSATION_ITEM (node_array->pdata[idx]), TRUE);

  return TRUE;
}

void
stamp_conversation_list_commit_trash (StampConversationList *self)
{
  if (!self->trash_array || !self->trash_entries)
    return;

  for (GList *l = self->trash_entries; l; l = l->next) {
    StampTrashEntry *entry = l->data;
    CamelFolder *trash_folder;
    g_autoptr (GPtrArray) uid_array = NULL;

    trash_folder = stamp_account_get_mail_trash_folder (entry->account);
    if (!entry->source_folder || !trash_folder || entry->uids->len == 0)
      continue;

    uid_array = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < entry->uids->len; i++)
      g_ptr_array_add (uid_array, g_strdup (g_ptr_array_index (entry->uids, i)));

    for (gint idx = uid_array->len - 1; idx >= 0; idx--)
      camel_folder_set_message_flags (entry->source_folder,
                                      g_ptr_array_index (uid_array, idx),
                                      CAMEL_MESSAGE_SEEN, CAMEL_MESSAGE_SEEN);

    if (!self->transfer_cancellable)
      self->transfer_cancellable = g_cancellable_new ();
    camel_folder_transfer_messages_to (entry->source_folder, uid_array,
                                       trash_folder, TRUE,
                                       G_PRIORITY_DEFAULT, self->transfer_cancellable,
                                       on_transfer_messages_to, self);
  }

  g_clear_pointer (&self->trash_array, g_ptr_array_unref);
  g_clear_list (&self->trash_entries, trash_entry_free);
  g_clear_object (&self->transfer_cancellable);
}

void
stamp_conversation_list_finalize_trash (StampConversationList *self,
                                        gboolean               select_next)
{
  StampConversationItem *selected_item;
  gboolean keep_selection;

  if (!self->trash_array || !self->trash_entries)
    return;

  selected_item = gtk_single_selection_get_selected_item (self->single_selection);
  keep_selection = !self->selection_mode && selected_item &&
                   !g_ptr_array_find (self->trash_array, selected_item, NULL);

  stamp_conversation_list_commit_trash (self);

  if (keep_selection)
    return;

  if (self->selection_mode || !select_next)
    stamp_conversation_list_unselect (self);
  else
    stamp_conversation_list_select_relative (self, 1);
}

void
stamp_mail_conversation_list_search_contact (StampConversationList *self,
                                             const gchar           *mail)
{
  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), mail);
  gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (self->search_bar), TRUE);
}

void
stamp_conversation_list_undo_trash (StampConversationList *self)
{
  if (!self->trash_array)
    return;

  for (gint idx = 0; idx < self->trash_array->len; idx++) {
    StampConversationItem *item = STAMP_CONVERSATION_ITEM (self->trash_array->pdata[idx]);

    set_item_hidden (self, item, FALSE);
  }

  g_clear_pointer (&self->trash_array, g_ptr_array_unref);
  g_clear_list (&self->trash_entries, trash_entry_free);
}

GtkToggleButton *
stamp_conversation_list_get_sidebar_button (StampConversationList *self)
{
  return self->sidebar_button;
}

void
stamp_consersation_list_set_show_buttons (StampConversationList *self,
                                          gboolean               show)
{
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (self->normal_headerbar), show);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (self->selection_headerbar), show);
}

void
stamp_conversation_list_set_key_capture_widget (StampConversationList *self,
                                                gboolean               enable)
{
  gtk_search_bar_set_key_capture_widget (GTK_SEARCH_BAR (self->search_bar),
                                         enable ? self->conversation_view : NULL);
}

void
stamp_conversation_list_select_relative (StampConversationList *self,
                                         gint                   direction)
{
  GtkSelectionModel *model = GTK_SELECTION_MODEL (self->single_selection);
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (model));
  guint selected;

  if (self->selection_mode || n_items == 0)
    return;

  selected = gtk_single_selection_get_selected (self->single_selection);

  if (selected == GTK_INVALID_LIST_POSITION) {
    selected = (direction > 0) ? 0 : n_items - 1;
  } else {
    gint new_pos = (int)selected + direction;
    selected = CLAMP (new_pos, 0, (int)n_items - 1);
  }

  gtk_selection_model_select_item (model, selected, TRUE);
}

StampConversationItem *
stamp_conversation_list_get_adjacent_item (StampConversationList *self,
                                           gint                   offset)
{
  GListModel *model = G_LIST_MODEL (self->single_selection);
  guint n_items = g_list_model_get_n_items (model);
  guint selected;
  gint new_pos;

  if (n_items == 0)
    return NULL;

  selected = gtk_single_selection_get_selected (self->single_selection);

  if (selected == GTK_INVALID_LIST_POSITION)
    return NULL;

  new_pos = (int)selected + offset;

  if (new_pos < 0 || new_pos >= (int)n_items)
    return NULL;

  return g_list_model_get_item (model, new_pos);
}

void
stamp_conversation_list_junk (StampConversationList *self,
                              StampConversationItem *item)
{
  g_autoptr (GPtrArray) node_array = g_ptr_array_new ();
  CamelFolder *folder;
  CamelFolder *junk_folder;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) uid_array = NULL;

  if (self->selection_mode) {
    GtkBitset *selected_items = self->selected;
    GtkBitsetIter iter;
    guint32 current_item_position;

    gtk_bitset_iter_init_first (&iter, selected_items, &current_item_position);

    while (gtk_bitset_iter_is_valid (&iter)) {
      g_autoptr (StampConversationItem) child_item = STAMP_CONVERSATION_ITEM (g_list_model_get_item (G_LIST_MODEL (self->multi_selection), current_item_position));

      g_ptr_array_add (node_array, child_item);
      gtk_bitset_iter_next (&iter, &current_item_position);
    }

    gtk_bitset_remove_all (self->selected);
    update_selection_title (self);
    refresh_checkboxes (self);
  } else {
    if (!item)
      item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));

    if (!item)
      return;

    g_ptr_array_add (node_array, item);
  }

  folder = self->folder;
  junk_folder = stamp_account_get_mail_junk_folder (self->account);

  uid_array = g_ptr_array_new_with_free_func (g_free);

  for (gint idx = 0; idx < node_array->len; idx++) {
    StampConversationItem *child_item = STAMP_CONVERSATION_ITEM (node_array->pdata[idx]);
    CamelFolderThreadNode *child_node = stamp_conversation_item_get_node (child_item);
    g_autoptr (GPtrArray) folder_uids = NULL;

    stamp_conversation_item_set_hidden (child_item, TRUE);
    folder_uids = collect_uids_for_folder (child_node, folder);

    for (guint ui = 0; ui < folder_uids->len; ui++)
      g_ptr_array_add (uid_array, g_strdup (g_ptr_array_index (folder_uids, ui)));
  }

  g_list_model_items_changed (G_LIST_MODEL (self->list_store),
                              0,
                              g_list_model_get_n_items (G_LIST_MODEL (self->list_store)),
                              g_list_model_get_n_items (G_LIST_MODEL (self->list_store)));

  if (uid_array->len == 0) {
    g_warning ("%s: No uids from current folder, abort", G_STRFUNC);
    return;
  }

  if (self->transfer_cancellable) {
    g_cancellable_cancel (self->transfer_cancellable);
    g_clear_object (&self->transfer_cancellable);
  }
  self->transfer_cancellable = g_cancellable_new ();

  camel_folder_transfer_messages_to (folder, uid_array, junk_folder, TRUE, G_PRIORITY_DEFAULT, self->transfer_cancellable, on_transfer_messages_to, self);

  stamp_conversation_list_unselect (self);
}

static gboolean
archive_conversation (StampConversationList *self,
                      StampConversationItem *item)
{
  StampAccount *account = get_item_account (self, item);
  CamelFolder *archives_folder;
  GList single_folder = { .data = self->folder };
  GList *folders = self->is_collection ? self->collection_folders : &single_folder;
  gboolean moved = FALSE;

  if (!account)
    return FALSE;

  archives_folder = stamp_account_get_mail_archives_folder (account);
  if (!archives_folder) {
    g_warning ("%s: No archives folder available, abort", G_STRFUNC);
    return FALSE;
  }

  for (GList *iter = folders; iter; iter = iter->next) {
    CamelFolder *folder = iter->data;
    g_autoptr (GPtrArray) uids = NULL;

    if (!folder || folder == archives_folder)
      continue;

    uids = collect_uids_for_folder (stamp_conversation_item_get_node (item), folder);
    if (uids->len == 0)
      continue;

    camel_folder_transfer_messages_to (folder, uids, archives_folder, TRUE, G_PRIORITY_DEFAULT, self->transfer_cancellable, on_transfer_messages_to, self);
    moved = TRUE;
  }

  return moved;
}

void
stamp_conversation_list_archive (StampConversationList *self,
                                 StampConversationItem *item)
{
  g_autoptr (GPtrArray) node_array = g_ptr_array_new_with_free_func (g_object_unref);
  gboolean moved = FALSE;

  if (self->selection_mode) {
    GtkBitset *selected_items = self->selected;
    GtkBitsetIter iter;
    guint32 current_item_position;

    gtk_bitset_iter_init_first (&iter, selected_items, &current_item_position);

    while (gtk_bitset_iter_is_valid (&iter)) {
      g_autoptr (StampConversationItem) child_item = STAMP_CONVERSATION_ITEM (g_list_model_get_item (G_LIST_MODEL (self->multi_selection), current_item_position));

      g_ptr_array_add (node_array, g_steal_pointer (&child_item));
      gtk_bitset_iter_next (&iter, &current_item_position);
    }

    gtk_bitset_remove_all (self->selected);
    update_selection_title (self);
    refresh_checkboxes (self);
  } else {
    if (!item)
      item = STAMP_CONVERSATION_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (self->single_selection)));

    if (!item)
      return;

    g_ptr_array_add (node_array, g_object_ref (item));
  }

  if (node_array->len == 0)
    return;

  g_cancellable_cancel (self->transfer_cancellable);
  g_clear_object (&self->transfer_cancellable);

  self->transfer_cancellable = g_cancellable_new ();

  for (guint idx = 0; idx < node_array->len; idx++) {
    StampConversationItem *child_item = g_ptr_array_index (node_array, idx);

    if (archive_conversation (self, child_item)) {
      stamp_conversation_item_set_hidden (child_item, TRUE);
      moved = TRUE;
    }
  }

  if (!moved)
    return;

  g_list_model_items_changed (G_LIST_MODEL (self->list_store), 0, g_list_model_get_n_items (G_LIST_MODEL (self->list_store)), g_list_model_get_n_items (G_LIST_MODEL (self->list_store)));

  stamp_conversation_list_unselect (self);
}
