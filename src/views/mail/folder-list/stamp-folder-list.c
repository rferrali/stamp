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

#include "stamp-folder-list.h"

#include <camel/camel.h>
#include <glib/gi18n.h>

#include "stamp-account-item.h"
#include "stamp-collection-item.h"
#include "stamp-conversation-item.h"
#include "stamp-conversation-list.h"
#include "stamp-folder-item.h"
#include "stamp-folder-row.h"
#include "stamp-helper.h"
#include "stamp-session.h"
#include "stamp-settings.h"

struct _StampFolderList {
  AdwBin parent_instance;

  GtkCustomSorter *sorter;
  GtkSortListModel *sort_list_model;
  GtkSingleSelection *selection;

  gboolean already_selected;
  GListStore *list_store;
  GPtrArray *expand_queue;
  guint expand_handler;

  StampCollectionItem *collection;
  StampItem *collection_group;
  StampConversationList *conversation_list;
};

G_DEFINE_FINAL_TYPE (StampFolderList, stamp_folder_list, ADW_TYPE_BIN);

enum {
  FOLDER_SELECTED,
  COLLECTION_SELECTED,
  LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = { 0 };

static void
on_selection_changed (GtkSelectionModel *selection,
                      guint              position,
                      guint              n_items,
                      gpointer           user_data)
{
  gpointer selected_item = gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (selection));
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  GtkTreeListRow *row;
  StampItem *item;

  if (!selected_item)
    return;

  row = GTK_TREE_LIST_ROW (selected_item);
  item = STAMP_ITEM (gtk_tree_list_row_get_item (row));

  if (STAMP_IS_COLLECTION_ITEM (item)) {
    g_signal_emit (self, signals[COLLECTION_SELECTED], 0, item);
  } else if (STAMP_IS_FOLDER_ITEM (item)) {
    StampAccount *account = stamp_item_get_account (item);
    const gchar *full_name = stamp_folder_item_get_full_name (STAMP_FOLDER_ITEM (item));

    g_settings_set (STAMP_SETTINGS_MAIL, STAMP_PREFS_MAIL_SELECTED_FOLDER, "(ss)", stamp_account_get_name (account), full_name);

    g_signal_emit (self, signals[FOLDER_SELECTED], 0, account, full_name);
  }
}

static void
insert_item (StampFolderList  *self,
             StampAccountItem *item)
{
  gchar **saved_order = g_settings_get_strv (STAMP_SETTINGS, STAMP_PREFS_ACCOUNT_ORDER);
  gint pos = -1;
  gint saved_pos = 0;
  const gchar *name = stamp_item_get_name (STAMP_ITEM (item));

  while (saved_order[saved_pos]) {
    if (g_strcmp0 (name, saved_order[saved_pos]) == 0) {
      pos = saved_pos;
      break;
    }
    saved_pos++;
  }

  if (pos == -1) {
    g_list_store_append (self->list_store, item);
  } else {
    gint insert_index = 0;
    guint list_len = g_list_model_get_n_items (G_LIST_MODEL (self->list_store));

    for (guint idx = 0; idx < list_len; idx++) {
      StampItem *cur_item = g_list_model_get_item (G_LIST_MODEL (self->list_store), idx);
      const gchar *cur_name;

      if (!STAMP_IS_ACCOUNT_ITEM (cur_item))
        continue;

      cur_name = stamp_item_get_name (cur_item);

      for (gint j = 0; j < pos; j++)
        if (g_strcmp0 (saved_order[j], cur_name) == 0)
          insert_index++;
    }

    g_list_store_insert (self->list_store, insert_index, item);
  }
}

static const gchar *
find_inbox_full_name (GListStore *store)
{
  guint list_len = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (guint idx = 0; idx < list_len; idx++) {
    g_autoptr (StampItem) item = STAMP_ITEM (g_list_model_get_item (G_LIST_MODEL (store), idx));
    GListStore *child_store;

    if (STAMP_IS_FOLDER_ITEM (item)) {
      gint type = stamp_folder_item_get_flags (STAMP_FOLDER_ITEM (item)) & CAMEL_FOLDER_TYPE_MASK;

      if (type == CAMEL_FOLDER_TYPE_INBOX)
        return stamp_folder_item_get_full_name (STAMP_FOLDER_ITEM (item));
    }

    child_store = stamp_item_get_list_store (item);
    if (child_store) {
      const gchar *ret = find_inbox_full_name (child_store);

      if (ret)
        return ret;
    }
  }

  return NULL;
}

static void
sync_collection_for_account (StampFolderList  *self,
                             StampAccountItem *account_item)
{
  StampAccount *account = stamp_item_get_account (STAMP_ITEM (account_item));
  const gchar *full_name;
  GList *sources;

  if (!self->collection)
    return;

  full_name = find_inbox_full_name (stamp_item_get_list_store (STAMP_ITEM (account_item)));
  if (!full_name)
    return;

  for (sources = stamp_collection_item_get_sources (self->collection); sources; sources = sources->next) {
    StampCollectionSource *source = sources->data;

    if (source->account == account)
      return;
  }

  stamp_collection_item_add_source (self->collection, account, full_name);
  stamp_collection_item_load (self->collection);
}

static void
on_account_item_changed (GtkWidget *account_item,
                         gpointer   user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);

  sync_collection_for_account (self, STAMP_ACCOUNT_ITEM (account_item));
}

static guint
count_account_items (StampFolderList *self)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->list_store));
  guint count = 0;

  for (guint idx = 0; idx < n; idx++) {
    g_autoptr (StampItem) item = STAMP_ITEM (g_list_model_get_item (G_LIST_MODEL (self->list_store), idx));

    if (STAMP_IS_ACCOUNT_ITEM (item))
      count++;
  }

  return count;
}

static guint
find_collection_position (StampFolderList *self)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->list_store));

  for (guint idx = 0; idx < n; idx++) {
    g_autoptr (StampItem) item = STAMP_ITEM (g_list_model_get_item (G_LIST_MODEL (self->list_store), idx));

    if (item == self->collection_group)
      return idx;
  }

  return G_MAXUINT;
}

static void
update_collection_visibility (StampFolderList *self)
{
  guint pos;

  if (!self->collection || !self->collection_group)
    return;

  pos = find_collection_position (self);

  if (count_account_items (self) > 1) {
    if (pos == G_MAXUINT)
      g_list_store_insert (self->list_store, 0, self->collection_group);
  } else if (pos != G_MAXUINT) {
    g_list_store_remove (self->list_store, pos);
  }
}

static void
clear_conversation_list_if_needed (StampFolderList *self,
                                   StampAccount    *removed_account)
{
  StampConversationList *cl = self->conversation_list;

  if (!cl)
    return;

  if (stamp_conversation_list_is_collection (cl)) {
    if (count_account_items (self) <= 1)
      stamp_conversation_list_clear (cl);
  } else if (stamp_conversation_list_get_account (cl) == removed_account) {
    stamp_conversation_list_clear (cl);
  }
}

static void
on_mail_added (GtkWidget        *object,
               StampAccount     *account,
               StampMailService *service,
               gpointer          user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  g_autoptr (StampAccountItem) item = NULL;
  guint len = g_list_model_get_n_items (G_LIST_MODEL (self->list_store));

  /* Safety: Do not account twice, shouldn't happen at all... */
  for (guint idx = 0; idx < len; idx++) {
    g_autoptr (StampItem) check_item = g_list_model_get_item (G_LIST_MODEL (self->list_store), idx);

    if (!STAMP_IS_ACCOUNT_ITEM (check_item))
      continue;

    if (stamp_item_get_account (check_item) == account) {
      return;
    }
  }

  item = stamp_account_item_new (account);
  stamp_account_item_load (item);
  insert_item (self, item);

  g_signal_connect_object (item, "account-item-changed", G_CALLBACK (on_account_item_changed), self, G_CONNECT_DEFAULT);

  update_collection_visibility (self);
}

static void
on_mail_removed (GtkWidget        *object,
                 StampAccount     *account,
                 StampMailService *service,
                 gpointer          user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  guint len = g_list_model_get_n_items (G_LIST_MODEL (self->list_store));

  if (self->collection)
    stamp_collection_item_remove_source (self->collection, account);

  for (guint idx = 0; idx < len; idx++) {
    g_autoptr (StampItem) item = g_list_model_get_item (G_LIST_MODEL (self->list_store), idx);

    if (!STAMP_IS_ACCOUNT_ITEM (item))
      continue;

    if (stamp_item_get_account (item) == account) {
      g_list_store_remove (self->list_store, idx);
      break;
    }
  }

  update_collection_visibility (self);
  clear_conversation_list_if_needed (self, account);
}

static void
on_stamp_folder_list_account_added (GObject      *object,
                                    StampAccount *account,
                                    gpointer      user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  g_autoptr (StampAccountItem) account_item = NULL;
  StampMailService *mail_service;

  g_signal_connect_object (account, "mail-added", G_CALLBACK (on_mail_added), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (account, "mail-removed", G_CALLBACK (on_mail_removed), self, G_CONNECT_DEFAULT);

  mail_service = stamp_account_get_mail_service (account);
  if (!mail_service || !stamp_mail_service_get_enabled (mail_service))
    return;

  account_item = stamp_account_item_new (account);
  stamp_account_item_load (account_item);
  insert_item (self, account_item);

  g_signal_connect_object (account_item, "account-item-changed", G_CALLBACK (on_account_item_changed), self, G_CONNECT_DEFAULT);

  update_collection_visibility (self);
}

static void
on_stamp_folder_list_account_removed (GObject      *object,
                                      StampAccount *account,
                                      gpointer      user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  gint len = g_list_model_get_n_items (G_LIST_MODEL (self->list_store));

  if (self->collection)
    stamp_collection_item_remove_source (self->collection, account);

  for (gint idx = 0; idx < len; idx++) {
    g_autoptr (StampItem) item = STAMP_ITEM (g_list_model_get_item (G_LIST_MODEL (self->list_store), idx));

    if (!STAMP_IS_ACCOUNT_ITEM (item))
      continue;

    if (stamp_item_get_account (item) == account) {
      g_list_store_remove (self->list_store, idx);
      break;
    }
  }

  update_collection_visibility (self);
  clear_conversation_list_if_needed (self, account);
}

static void
on_row_expanded (GtkTreeListRow *row,
                 GParamSpec     *pspec,
                 gpointer        user_data)
{
  StampItem *item = STAMP_ITEM (gtk_tree_list_row_get_item (row));
  StampAccount *account = stamp_item_get_account (STAMP_ITEM (item));
  g_autofree char *settings_path = NULL;
  g_autoptr (GSettings) account_settings = NULL;
  g_auto (GStrv) folders = NULL;
  g_auto (GStrv) new_folders = NULL;
  const gchar *full_name = stamp_folder_item_get_full_name (STAMP_FOLDER_ITEM (item));

  settings_path = g_strconcat ("/org/tabos/stamp/mail/accounts/", stamp_account_get_uid (account), "/", NULL);
  account_settings = g_settings_new_with_path ("org.tabos.stamp.mail.accounts", settings_path);
  folders = g_settings_get_strv (account_settings, "expanded-folders");

  if (gtk_tree_list_row_get_expanded (row)) {
    new_folders = g_strv_append ((const char * const *)folders, full_name);
  } else {
    new_folders = g_strv_remove ((const char * const *)folders, full_name);
  }

  g_settings_set_strv (account_settings, "expanded-folders", (const char * const *)new_folders);
}

static void
expand_idle (gpointer user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);

  for (guint i = 0; i < self->expand_queue->len; i++)
    gtk_tree_list_row_set_expanded (self->expand_queue->pdata[i], TRUE);

  g_ptr_array_set_size (self->expand_queue, 0);
  self->expand_handler = 0;
}

static void
on_group_expanded (GtkTreeListRow *row,
                   GParamSpec     *pspec,
                   gpointer        user_data)
{
  g_settings_set_boolean (STAMP_SETTINGS_MAIL, STAMP_PREFS_MAIL_ALL_MAILBOXES_EXPANDED,
                          gtk_tree_list_row_get_expanded (row));
}

static void
on_bind_folder (GtkListItemFactory *factory,
                GtkListItem        *list_item,
                gpointer            user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  StampFolderRow *folder_row;
  StampItem *item;
  StampAccount *account;
  GtkTreeListRow *row;
  GtkWidget *expander;
  g_autoptr (GSettings) account_settings = NULL;
  g_autofree char *settings_path = NULL;

  expander = gtk_list_item_get_child (list_item);
  row = GTK_TREE_LIST_ROW (gtk_list_item_get_item (list_item));
  item = STAMP_ITEM (gtk_tree_list_row_get_item (row));

  gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), row);

  folder_row = STAMP_FOLDER_ROW (gtk_tree_expander_get_child (GTK_TREE_EXPANDER (expander)));

  stamp_folder_row_bind (folder_row, item);

  if (STAMP_IS_COLLECTION_ITEM (item))
    return;

  if (item == self->collection_group) {
    gtk_widget_add_css_class (GTK_WIDGET (folder_row), "bold");

    if (g_settings_get_boolean (STAMP_SETTINGS_MAIL, STAMP_PREFS_MAIL_ALL_MAILBOXES_EXPANDED)) {
      g_ptr_array_add (self->expand_queue, g_object_ref (row));

      if (self->expand_handler == 0)
        self->expand_handler = g_idle_add_once (expand_idle, self);
    }

    g_signal_connect_object (row, "notify::expanded", G_CALLBACK (on_group_expanded), self, G_CONNECT_DEFAULT);
    return;
  }

  /* FIXME: Optimize the following code */
  account = stamp_item_get_account (STAMP_ITEM (item));

  settings_path = g_strconcat ("/org/tabos/stamp/mail/accounts/", stamp_account_get_uid (account), "/", NULL);
  account_settings = g_settings_new_with_path ("org.tabos.stamp.mail.accounts", settings_path);

  if (STAMP_IS_FOLDER_ITEM (item)) {
    const gchar *full_name = stamp_folder_item_get_full_name (STAMP_FOLDER_ITEM (item));
    g_auto (GStrv) expanded_folders = g_settings_get_strv (account_settings, "expanded-folders");

    if (!self->already_selected) {
      g_autofree char *account_name = NULL;
      g_autofree char *folder_name = NULL;

      g_settings_get (STAMP_SETTINGS_MAIL, STAMP_PREFS_MAIL_SELECTED_FOLDER, "(ss)", &account_name, &folder_name);
      if (g_strcmp0 (stamp_account_get_name (account), account_name) == 0 && g_strcmp0 (full_name, folder_name) == 0) {
        gtk_single_selection_set_selected (self->selection, gtk_list_item_get_position (list_item));
        self->already_selected = TRUE;
      }
    }

    if (g_strv_contains ((const char **)expanded_folders, full_name)) {
      g_ptr_array_add (self->expand_queue, g_object_ref (row));

      if (self->expand_handler == 0)
        self->expand_handler = g_idle_add_once (expand_idle, self);
    }

    g_signal_connect_object (row, "notify::expanded", G_CALLBACK (on_row_expanded), self, G_CONNECT_DEFAULT);
  } else {
    /* Set initial account expanded state */
    g_settings_bind (account_settings, "expanded", row, "expanded", G_SETTINGS_BIND_DEFAULT | G_SETTINGS_BIND_GET_NO_CHANGES);
  }
}

static GListModel *
get_child (void     *item,
           gpointer  user_data)
{
  if (STAMP_IS_ITEM (item)) {
    GListModel *model = G_LIST_MODEL (stamp_item_get_list_store (STAMP_ITEM (item)));

    return model ? g_object_ref (model) : NULL;
  }

  return NULL;
}

static gint
folders_sorter (gconstpointer a,
                gconstpointer b,
                gpointer      user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  StampItem *item_a = (StampItem *)(a);
  StampItem *item_b = (StampItem *)(b);

  if (self->collection_group) {
    if (item_a == self->collection_group)
      return -1;
    if (item_b == self->collection_group)
      return 1;
  }

  if (STAMP_IS_COLLECTION_ITEM (item_a) && STAMP_IS_COLLECTION_ITEM (item_b)) {
    return g_strcmp0 (stamp_item_get_name (item_a), stamp_item_get_name (item_b));
  } else if (STAMP_IS_COLLECTION_ITEM (item_a)) {
    return -1;
  } else if (STAMP_IS_COLLECTION_ITEM (item_b)) {
    return 1;
  }

  if (STAMP_IS_FOLDER_ITEM (item_a) && STAMP_IS_FOLDER_ITEM (item_b)) {
    const gchar *name_a;
    const gchar *name_b;
    gint rank_a = stamp_folder_item_get_order_rank (STAMP_FOLDER_ITEM (item_a));
    gint rank_b = stamp_folder_item_get_order_rank (STAMP_FOLDER_ITEM (item_b));

    if (rank_a != rank_b)
      return rank_a - rank_b;

    name_a = stamp_item_get_name (item_a);
    name_b = stamp_item_get_name (item_b);

    return g_utf8_collate (name_a ? name_a : "", name_b ? name_b : "");
  } else {
    g_debug ("%s: Failed to compare unknown combination", G_STRFUNC);
  }

  return 0;
}

static void
on_row_released (GtkGestureClick *gesture,
                 gint             n_press,
                 gdouble          x,
                 gdouble          y,
                 gpointer         user_data)
{
  GtkListItem *list_item = GTK_LIST_ITEM (user_data);
  GtkTreeListRow *row = gtk_list_item_get_item (list_item);
  StampItem *item;

  if (!row)
    return;

  item = STAMP_ITEM (gtk_tree_list_row_get_item (row));
  if (!STAMP_IS_ACCOUNT_ITEM (item))
    return;

  if (gtk_tree_list_row_is_expandable (row)) {
    gboolean expanded = gtk_tree_list_row_get_expanded (row);
    gtk_tree_list_row_set_expanded (row, !expanded);
  }
}

static GdkContentProvider *
on_drag_prepare (GtkDragSource *source,
                 gdouble        x,
                 gdouble        y,
                 gpointer       user_data)
{
  GtkListItem *list_item = GTK_LIST_ITEM (user_data);
  GtkTreeExpander *expander = GTK_TREE_EXPANDER (gtk_list_item_get_child (list_item));
  StampItem *item = STAMP_ITEM (gtk_tree_expander_get_item (expander));

  if (STAMP_IS_ACCOUNT_ITEM (item)) {
    g_type_ensure (STAMP_TYPE_ITEM);
    return gdk_content_provider_new_typed (STAMP_TYPE_ITEM, item);
  }

  return NULL;
}

static guint
find_position (GListStore *store,
               gpointer    item)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (guint i = 0; i < n; i++) {
    gpointer it = g_list_model_get_item (G_LIST_MODEL (store), i);

    if (it == item) {
      g_object_unref (it);
      return i;
    }

    g_object_unref (it);
  }

  return GTK_INVALID_LIST_POSITION;
}

static void
save_account_order (StampFolderList *self)
{
  g_autoptr (GSettings) settings = g_settings_new ("org.tabos.stamp");
  GListStore *store = self->list_store;
  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  g_auto (GStrv) arr = g_new (gchar *, n + 1);
  gint array_index = 0;

  for (guint i = 0; i < n; i++) {
    gpointer it = g_list_model_get_item (G_LIST_MODEL (store), i);

    if (STAMP_IS_ACCOUNT_ITEM (it)) {
      arr[array_index++] = g_strdup (stamp_item_get_name (STAMP_ITEM (it)));
    }

    g_object_unref (it);
  }

  arr[array_index] = NULL;
  g_settings_set_strv (settings, "account-order", (const gchar * const *)arr);
}

static gboolean
on_account_drop (GtkDropTarget *target,
                 const GValue  *value,
                 gdouble        x,
                 gdouble        y,
                 gpointer       user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  GListStore *store = G_LIST_STORE (self->list_store);
  StampItem *src_item = g_value_get_object (value);
  StampFolderRow *row = STAMP_FOLDER_ROW (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target)));
  GtkListItem *list_item = g_object_get_data (G_OBJECT (row), "list-item");
  GtkTreeExpander *expander = GTK_TREE_EXPANDER (gtk_list_item_get_child (list_item));
  StampItem *dest_item = STAMP_ITEM (gtk_tree_expander_get_item (expander));
  guint src_pos;
  guint dest_pos;
  gpointer item;

  if (!STAMP_IS_ACCOUNT_ITEM (dest_item))
    return FALSE;

  src_pos = find_position (store, src_item);
  dest_pos = find_position (store, dest_item);

  if (src_pos == GTK_INVALID_LIST_POSITION ||
      dest_pos == GTK_INVALID_LIST_POSITION)
    return FALSE;

  item = g_list_model_get_item (G_LIST_MODEL (store), src_pos);

  g_list_store_remove (store, src_pos);
  g_list_store_insert (store, dest_pos, item);

  g_object_unref (item);

  save_account_order (self);

  return TRUE;
}

static gboolean
on_conversation_drop (GtkDropTarget *target,
                      const GValue  *value,
                      gdouble        x,
                      gdouble        y,
                      gpointer       user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  StampConversationItem *conv_item = g_value_get_object (value);
  StampFolderRow *row = STAMP_FOLDER_ROW (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target)));
  GtkListItem *list_item = g_object_get_data (G_OBJECT (row), "list-item");
  GtkTreeExpander *expander = GTK_TREE_EXPANDER (gtk_list_item_get_child (list_item));
  StampItem *folder_item = STAMP_ITEM (gtk_tree_expander_get_item (expander));

  if (!STAMP_IS_FOLDER_ITEM (folder_item) || !self->conversation_list)
    return FALSE;

  stamp_conversation_list_move_conversation (self->conversation_list,
                                             conv_item,
                                             stamp_folder_item_get_full_name (STAMP_FOLDER_ITEM (folder_item)));

  return TRUE;
}

static gboolean
on_drop (GtkDropTarget *target,
         const GValue  *value,
         gdouble        x,
         gdouble        y,
         gpointer       user_data)
{
  if (G_VALUE_HOLDS (value, STAMP_TYPE_ITEM))
    return on_account_drop (target, value, x, y, user_data);

  if (G_VALUE_HOLDS (value, STAMP_TYPE_CONVERSATION_ITEM))
    return on_conversation_drop (target, value, x, y, user_data);

  return FALSE;
}

static gboolean
on_accept (GtkDropTarget *self,
           GdkDrop       *drop,
           gpointer       user_data)
{
  GtkListItem *list_item = GTK_LIST_ITEM (user_data);
  GtkTreeExpander *expander = GTK_TREE_EXPANDER (gtk_list_item_get_child (list_item));
  StampItem *item = STAMP_ITEM (gtk_tree_expander_get_item (expander));
  GdkContentFormats *formats = gdk_drop_get_formats (drop);

  if (STAMP_IS_ACCOUNT_ITEM (item) && gdk_content_formats_contain_gtype (formats, STAMP_TYPE_ITEM))
    return TRUE;

  if (STAMP_IS_FOLDER_ITEM (item) && gdk_content_formats_contain_gtype (formats, STAMP_TYPE_CONVERSATION_ITEM))
    return TRUE;

  return FALSE;
}

static void
on_drag_begin (GtkDragSource  *source,
               GdkDrag        *drag,
               StampFolderRow *self)
{
  /* Set the widget as the drag icon */
  g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (GTK_WIDGET (self));

  gtk_drag_source_set_icon (source, paintable, 0, 0);
}

static void
on_setup_folder (GtkListItemFactory *factory,
                 GtkListItem        *list_item,
                 gpointer            user_data)
{
  StampFolderList *self = STAMP_FOLDER_LIST (user_data);
  GtkWidget *row;
  GtkWidget *expander;
  GtkGesture *click = gtk_gesture_click_new ();
  GtkDragSource *drag = gtk_drag_source_new ();
  GtkDropTarget *drop = gtk_drop_target_new (G_TYPE_INVALID, GDK_ACTION_MOVE);

  gtk_list_item_set_activatable (list_item, TRUE);

  expander = gtk_tree_expander_new ();
  row = stamp_folder_row_new ();
  gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), row);

  gtk_list_item_set_child (list_item, expander);

  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  g_signal_connect (click, "released", G_CALLBACK (on_row_released), list_item);
  gtk_widget_add_controller (expander, GTK_EVENT_CONTROLLER (click));

  gtk_drag_source_set_actions (drag, GDK_ACTION_MOVE);
  g_signal_connect (drag, "prepare", G_CALLBACK (on_drag_prepare), list_item);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), row);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (drag));

  gtk_drop_target_set_gtypes (drop,
                              (GType[2]) { STAMP_TYPE_ITEM, STAMP_TYPE_CONVERSATION_ITEM },
                              2);
  g_object_set_data (G_OBJECT (row), "list-item", list_item);
  g_signal_connect (drop, "accept", G_CALLBACK (on_accept), list_item);
  g_signal_connect (drop, "drop", G_CALLBACK (on_drop), self);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (drop));
}

void
stamp_folder_list_set_conversation_list (StampFolderList       *self,
                                         StampConversationList *conversation_list)
{
  g_return_if_fail (STAMP_IS_FOLDER_LIST (self));

  self->conversation_list = conversation_list;
}

static void
stamp_folder_list_dispose (GObject *object)
{
  StampFolderList *self = STAMP_FOLDER_LIST (object);

  g_clear_object (&self->list_store);

  g_clear_handle_id (&self->expand_handler, g_source_remove);
  g_clear_pointer (&self->expand_queue, g_ptr_array_unref);
  g_clear_object (&self->collection);
  g_clear_object (&self->collection_group);

  self->conversation_list = NULL;

  G_OBJECT_CLASS (stamp_folder_list_parent_class)->dispose (object);
}

static void
stamp_folder_list_class_init (StampFolderListClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/folder-list/stamp-folder-list.ui");

  object_class->dispose = stamp_folder_list_dispose;

  gtk_widget_class_bind_template_child (widget_class, StampFolderList, selection);
  gtk_widget_class_bind_template_child (widget_class, StampFolderList, sorter);
  gtk_widget_class_bind_template_child (widget_class, StampFolderList, sort_list_model);

  gtk_widget_class_bind_template_callback (widget_class, on_selection_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_setup_folder);
  gtk_widget_class_bind_template_callback (widget_class, on_bind_folder);

  signals[FOLDER_SELECTED] = g_signal_new ("folder-selected", G_OBJECT_CLASS_TYPE (klass),
                                           G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                           0, NULL, NULL, NULL,
                                           G_TYPE_NONE,
                                           2, G_TYPE_OBJECT, G_TYPE_STRING);

  signals[COLLECTION_SELECTED] = g_signal_new ("collection-selected", G_OBJECT_CLASS_TYPE (klass),
                                               G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                               0, NULL, NULL, NULL,
                                               G_TYPE_NONE,
                                               1, STAMP_TYPE_COLLECTION_ITEM);
}

static void
stamp_folder_list_init (StampFolderList *self)
{
  StampSession *session = NULL;
  GtkTreeListModel *tree;
  GList *accounts;

  gtk_widget_init_template (GTK_WIDGET (self));

  self->list_store = g_list_store_new (STAMP_TYPE_ITEM);
  tree = gtk_tree_list_model_new (G_LIST_MODEL (self->list_store), FALSE, FALSE, get_child, g_object_ref (self), g_object_unref);

  gtk_sort_list_model_set_model (GTK_SORT_LIST_MODEL (self->sort_list_model), G_LIST_MODEL (tree));
  gtk_custom_sorter_set_sort_func (self->sorter, folders_sorter, self, NULL);

  session = stamp_session_get_default ();
  g_signal_connect_object (session, "account-added", G_CALLBACK (on_stamp_folder_list_account_added), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (session, "account-removed", G_CALLBACK (on_stamp_folder_list_account_removed), self, G_CONNECT_DEFAULT);

  self->expand_queue = g_ptr_array_new_with_free_func (g_object_unref);

  self->collection = stamp_collection_item_new (STAMP_COLLECTION_COMBINED_INBOX);
  stamp_item_set_name (STAMP_ITEM (self->collection), _("Combined Inbox"));
  stamp_item_set_icon_name (STAMP_ITEM (self->collection), "inbox-symbolic");

  self->collection_group = g_object_new (STAMP_TYPE_ITEM, NULL);
  stamp_item_set_name (self->collection_group, _("All Mailboxes"));
  stamp_item_set_list_store_type (self->collection_group, STAMP_TYPE_COLLECTION_ITEM);
  g_list_store_append (stamp_item_get_list_store (self->collection_group), STAMP_ITEM (self->collection));

  g_list_store_append (self->list_store, self->collection_group);

  for (accounts = stamp_session_get_accounts (session); accounts; accounts = accounts->next) {
    StampAccount *account = STAMP_ACCOUNT (accounts->data);
    StampMailService *mail_service = stamp_account_get_mail_service (account);

    if (!mail_service || !stamp_mail_service_get_enabled (mail_service))
      continue;

    on_stamp_folder_list_account_added (G_OBJECT (session), account, self);
  }

  update_collection_visibility (self);

  g_type_ensure (STAMP_TYPE_ITEM);
}

GtkWidget *
stamp_folder_list_new (void)
{
  return g_object_new (STAMP_TYPE_FOLDER_LIST, NULL);
}

void
stamp_folder_list_unselect (StampFolderList *self)
{
  gtk_selection_model_unselect_all (GTK_SELECTION_MODEL (self->selection));
}
