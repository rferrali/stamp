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

#include "stamp-book-list.h"

#include "stamp-account.h"
#include "stamp-book-account-item.h"
#include "stamp-book-item.h"
#include "stamp-book-row.h"
#include "stamp-item.h"
#include "stamp-session.h"

struct _StampBookList {
  AdwBin parent_instance;

  GListStore *list_store;
  GtkWidget *sorter;
  GtkWidget *sort_list_model;
  GtkSingleSelection *selection;
  GHashTable *account_table;

  gboolean already_selected;
  GSettings *settings;
};

G_DEFINE_FINAL_TYPE (StampBookList, stamp_book_list, ADW_TYPE_BIN);

enum {
  BOOK_SELECTED,
  LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = { 0 };
static void
on_setup_folder (GtkListItemFactory *factory,
                 GtkListItem        *list_item,
                 gpointer            user_data)
{
  GtkWidget *row;
  GtkWidget *expander;

  gtk_list_item_set_activatable (list_item, TRUE);

  expander = gtk_tree_expander_new ();
  row = stamp_book_row_new ();
  gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), row);

  gtk_list_item_set_child (list_item, expander);
}

static void
on_unbind_folder (GtkListItemFactory *factory,
                  GtkListItem        *list_item,
                  gpointer            user_data)
{
  stamp_book_row_unbind (STAMP_BOOK_ROW (gtk_tree_expander_get_child (GTK_TREE_EXPANDER (gtk_list_item_get_child (list_item)))));
}

static void
on_bind_folder (GtkListItemFactory *factory,
                GtkListItem        *list_item,
                gpointer            user_data)
{
  StampBookList *self = STAMP_BOOK_LIST (user_data);
  StampBookRow *book_row;
  StampItem *item;
  StampAccount *account = NULL;
  GtkTreeListRow *row;
  GtkWidget *expander;
  g_autoptr (GSettings) account_settings = NULL;
  g_autofree char *settings_path = NULL;
  GtkWidget *row_widget;
  guint depth;

  expander = gtk_list_item_get_child (list_item);
  row = GTK_TREE_LIST_ROW (gtk_list_item_get_item (list_item));
  item = STAMP_ITEM (gtk_tree_list_row_get_item (row));
  row_widget = gtk_widget_get_parent (GTK_WIDGET (expander));

  gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), row);

  book_row = STAMP_BOOK_ROW (gtk_tree_expander_get_child (GTK_TREE_EXPANDER (expander)));

  account = stamp_item_get_account (STAMP_ITEM (item));

  if (STAMP_IS_BOOK_ITEM (item)) {
    if (!self->already_selected) {
      EClient *client = stamp_book_item_get_client (STAMP_BOOK_ITEM (item));
      const gchar *full_name = e_source_get_display_name (e_client_get_source (client));
      g_autofree char *account_name = NULL;
      g_autofree char *book_name = NULL;

      g_settings_get (self->settings, "selected-book", "(ss)", &account_name, &book_name);
      if (g_strcmp0 (stamp_account_get_name (account), account_name) == 0 && g_strcmp0 (full_name, book_name) == 0) {
        gtk_single_selection_set_selected (self->selection, gtk_list_item_get_position (list_item));
        self->already_selected = TRUE;
      }
    }
  }

  stamp_book_row_bind (book_row, item);

  depth = gtk_tree_list_row_get_depth (row);

  gtk_widget_remove_css_class (row_widget, "tree-header");
  gtk_widget_remove_css_class (row_widget, "tree-child");

  if (depth == 0) {
    gtk_widget_add_css_class (row_widget, "tree-header");
  } else {
    gtk_widget_add_css_class (row_widget, "tree-child");
  }
}

static void
on_selection_changed (GtkSelectionModel *selection,
                      guint              position,
                      guint              n_items,
                      gpointer           user_data)
{
  gpointer selected_item = gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (selection));
  StampBookList *self = STAMP_BOOK_LIST (user_data);
  GtkTreeListRow *row;
  StampItem *item;

  if (!selected_item)
    return;

  row = GTK_TREE_LIST_ROW (selected_item);
  item = STAMP_ITEM (gtk_tree_list_row_get_item (row));

  if (STAMP_IS_BOOK_ITEM (item)) {
    StampAccount *account = stamp_item_get_account (item);
    EClient *client = stamp_book_item_get_client (STAMP_BOOK_ITEM (item));
    const gchar *full_name = e_source_get_display_name (e_client_get_source (client));

    g_settings_set (self->settings, "selected-book", "(ss)", stamp_account_get_name (account), full_name);
    g_signal_emit (self, signals[BOOK_SELECTED], 0, account, stamp_book_item_get_client (STAMP_BOOK_ITEM (item)));
  }
}

static void
stamp_book_list_class_init (StampBookListClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/contact/book-list/stamp-book-list.ui");

  gtk_widget_class_bind_template_child (widget_class, StampBookList, selection);
  gtk_widget_class_bind_template_child (widget_class, StampBookList, sorter);
  gtk_widget_class_bind_template_child (widget_class, StampBookList, sort_list_model);

  gtk_widget_class_bind_template_callback (widget_class, on_selection_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_setup_folder);
  gtk_widget_class_bind_template_callback (widget_class, on_bind_folder);
  gtk_widget_class_bind_template_callback (widget_class, on_unbind_folder);

  signals[BOOK_SELECTED] = g_signal_new ("book-selected", G_OBJECT_CLASS_TYPE (klass),
                                         G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE,
                                         2, G_TYPE_OBJECT, G_TYPE_POINTER);
}

/**
 * This function only takes care of a new book where account item
 * is not yet added to list store. Every other case is handled
 * in StampBookAccountItem.
 */
static void
on_book_added (GObject              *source,
               StampAccount         *account,
               StampContactsService *service,
               gpointer              user_data)
{
  StampBookList *self = STAMP_BOOK_LIST (user_data);
  g_autoptr (StampBookAccountItem) account_item = NULL;
  GListModel *model = G_LIST_MODEL (self->list_store);

  for (guint idx = 0; idx < g_list_model_get_n_items (model); idx++) {
    g_autoptr (StampBookAccountItem) item = STAMP_BOOK_ACCOUNT_ITEM (g_list_model_get_item (model, idx));

    if (stamp_item_get_account (STAMP_ITEM (item)) == account) {
      g_debug ("%s: Account already added, abort", G_STRFUNC);
      return;
    }
  }

  g_debug ("%s: New book added %s", G_STRFUNC, e_source_get_display_name (stamp_contacts_service_get_source (service)));
  account_item = stamp_book_account_item_new (account);
  g_list_store_append (self->list_store, account_item);
}

static void
on_book_removed (GObject              *source,
                 StampAccount         *account,
                 StampContactsService *service,
                 gpointer              user_data)
{
  StampBookList *self = STAMP_BOOK_LIST (user_data);
  g_autoptr (StampBookAccountItem) account_item = NULL;
  GListModel *model = G_LIST_MODEL (self->list_store);
  GPtrArray *books;

  books = stamp_account_get_books (account);
  if (books->len != 0)
    return;

  for (guint idx = 0; idx < g_list_model_get_n_items (model); idx++) {
    g_autoptr (StampBookAccountItem) item = STAMP_BOOK_ACCOUNT_ITEM (g_list_model_get_item (model, idx));

    if (stamp_item_get_account (STAMP_ITEM (item)) == account) {
      g_list_store_remove (self->list_store, idx);
      return;
    }
  }
}

static void
on_stamp_book_list_account_added (GObject      *object,
                                  StampAccount *account,
                                  gpointer      user_data)
{
  StampBookList *self = STAMP_BOOK_LIST (user_data);
  g_autoptr (StampBookAccountItem) account_item = NULL;
  GPtrArray *books;
  gboolean enabled = FALSE;

  g_signal_connect_object (account, "book-added", G_CALLBACK (on_book_added), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (account, "book-removed", G_CALLBACK (on_book_removed), self, G_CONNECT_DEFAULT);

  books = stamp_account_get_books (account);

  for (guint idx = 0; idx < books->len; idx++) {
    StampContactsService *service = g_ptr_array_index (books, idx);

    if (stamp_contacts_service_get_enabled (service)) {
      enabled = TRUE;
      break;
    }
  }

  if (!enabled)
    return;

  account_item = stamp_book_account_item_new (account);
  g_list_store_append (self->list_store, account_item);
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
books_sorter (gconstpointer a,
              gconstpointer b,
              gpointer      user_data)
{
  StampItem *item_a = (StampItem *)(a);
  StampItem *item_b = (StampItem *)(b);

  if (STAMP_IS_BOOK_ITEM (item_a) && STAMP_IS_BOOK_ITEM (item_b)) {
    return g_strcmp0 (stamp_item_get_name (item_a), stamp_item_get_name (item_b));
  } else if (STAMP_IS_BOOK_ACCOUNT_ITEM (item_a) && STAMP_IS_BOOK_ACCOUNT_ITEM (item_b)) {
    return g_strcmp0 (stamp_item_get_name (item_a), stamp_item_get_name (item_b));
  } else {
    g_debug ("%s: Failed to compare unknown combination", G_STRFUNC);
  }

  return 0;
}

static void
stamp_book_list_init (StampBookList *self)
{
  StampSession *session = NULL;
  GtkTreeListModel *tree;

  gtk_widget_init_template (GTK_WIDGET (self));

  session = stamp_session_get_default ();

  self->list_store = g_list_store_new (STAMP_TYPE_ITEM);
  tree = gtk_tree_list_model_new (G_LIST_MODEL (self->list_store), FALSE, TRUE, get_child, g_object_ref (self), g_object_unref);

  gtk_sort_list_model_set_model (GTK_SORT_LIST_MODEL (self->sort_list_model), G_LIST_MODEL (tree));
  gtk_custom_sorter_set_sort_func (GTK_CUSTOM_SORTER (self->sorter), books_sorter, NULL, NULL);

  g_signal_connect_object (session, "account-added", G_CALLBACK (on_stamp_book_list_account_added), self, G_CONNECT_DEFAULT);
  /* g_signal_connect_object (session, "account-removed", G_CALLBACK (on_stamp_book_list_account_removed), self, 0); */

  self->settings = g_settings_new ("org.tabos.stamp.contacts");

  self->account_table = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, g_object_unref);
}

void
stamp_book_list_unselect (StampBookList *self)
{
  gtk_selection_model_unselect_all (GTK_SELECTION_MODEL (self->selection));
}
