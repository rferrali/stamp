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

#include "stamp-contact-list.h"
#include "stamp-contact-row.h"
#include "stamp-type-builtins.h"

#include <glib/gi18n.h>

struct _StampContactList {
  AdwBreakpointBin parent_instance;

  GtkWidget *search_bar;
  GtkWidget *search_entry;
  GtkWidget *window_title;
  GtkWidget *progress;
  GtkWidget *stack;
  GtkWidget *sort_button;
  GtkWidget *sidebar_button;
  GtkWidget *normal_headerbar;

  GListStore *list_store;
  GtkSortListModel *sort_list_model;
  GtkFilterListModel *filter_list_model;
  GtkSorter *sorter;
  GtkFilter *filter;
  GtkSingleSelection *selection;

  EClient *client;
  StampAccount *account;
  guint progress_handle;
  GCancellable *cancellable;

  gboolean loading_done;
  guint loading_timeout_id;

  StampSortMode sort_mode;
};

G_DEFINE_FINAL_TYPE (StampContactList, stamp_contact_list, ADW_TYPE_BREAKPOINT_BIN);

typedef enum {
  PROP_SORT_MODE = 1,
} StampContactListProps;

static GParamSpec *properties[PROP_SORT_MODE + 1];

enum {
  CONTACT_SELECTED,
  LAST_SIGNAL
};

static gint signals[LAST_SIGNAL] = { 0 };


static void
on_show_loading (gpointer user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);

  if (!self->loading_done)
    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "loading");

  self->loading_timeout_id = 0;
}

static void
on_get_contacts (GObject      *object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);
  EBookClient *client = E_BOOK_CLIENT (object);
  g_autoslist (EContact) contacts = NULL;
  g_autoptr (GError) error = NULL;
  guint old;
  g_autoptr (GPtrArray) items = NULL;

  self->loading_done = TRUE;
  g_clear_handle_id (&self->loading_timeout_id, g_source_remove);

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "content");

  e_book_client_get_contacts_finish (client, res, &contacts, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Could not get contacts: %s", error->message);
    return;
  }

  old = g_list_model_get_n_items (G_LIST_MODEL (self->list_store));
  items = g_ptr_array_new_full (g_slist_length (contacts), g_object_unref);

  for (GSList *iter = contacts; iter && iter->data; iter = g_slist_next (iter)) {
    EContact *c = iter->data;
    StampContactItem *item = stamp_contact_item_new (c);
    g_ptr_array_add (items, item);
  }

  g_list_store_splice (self->list_store, 0, old, (gpointer *)items->pdata, items->len);
}

void
stamp_contact_list_load (StampContactList *self,
                         EClient          *client,
                         StampAccount     *account)
{
  const gchar *query = "(exists \"uid\")";

  self->account = account;
  self->client = client;

  if (!client)
    return;

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  self->cancellable = g_cancellable_new ();

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
  self->loading_done = FALSE;
  self->loading_timeout_id = g_timeout_add_once (500, on_show_loading, self);
  if (self->window_title) {
    ESource *source = e_client_get_source (E_CLIENT (client));

    adw_window_title_set_title (ADW_WINDOW_TITLE (self->window_title), e_source_get_display_name (source));
    adw_window_title_set_subtitle (ADW_WINDOW_TITLE (self->window_title), stamp_account_get_name (self->account));
  }

  gtk_single_selection_set_selected (self->selection, GTK_INVALID_LIST_POSITION);

  e_book_client_get_contacts (E_BOOK_CLIENT (client), query, self->cancellable, on_get_contacts, self);
}

static void
on_setup_list_item (GtkListItemFactory *factory,
                    GtkListItem        *list_item,
                    gpointer            user_data)
{
  GtkWidget *row = stamp_contact_row_new ();

  gtk_list_item_set_child (list_item, row);
}

static void
on_bind_list_item (GtkListItemFactory *factory,
                   GtkListItem        *list_item,
                   gpointer            user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);
  GtkWidget *row;
  StampContactItem *item;

  row = gtk_list_item_get_child (list_item);
  item = STAMP_CONTACT_ITEM (gtk_list_item_get_item (list_item));

  stamp_contact_row_bind_mail (STAMP_CONTACT_ROW (row), item, self);
}

static gint
sorter_func (gconstpointer a,
             gconstpointer b,
             gpointer      user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);
  StampContactItem *item1 = (StampContactItem *)(a);
  StampContactItem *item2 = (StampContactItem *)(b);

  if (self->sort_mode == SORT_MODE_GIVEN_NAME)
    return g_strcmp0 (stamp_contact_item_get_given_name (item1), stamp_contact_item_get_given_name (item2));

  return g_strcmp0 (stamp_contact_item_get_family_name (item1), stamp_contact_item_get_family_name (item2));
}

static gboolean
filter_func (gpointer object,
             gpointer user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);
  StampContactItem *item = STAMP_CONTACT_ITEM (object);
  const gchar *search_text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  const gchar *contact_name = stamp_contact_item_get_name (item);
  g_autofree char *search = NULL;
  g_autofree char *name = NULL;

  if (!search_text || strlen (search_text) == 0)
    return TRUE;

  if (!contact_name || strlen (contact_name) == 0)
    return FALSE;

  search = g_utf8_strdown (search_text, -1);
  name = g_utf8_strdown (contact_name, -1);

  return g_strrstr (name, search) != NULL;
}

static void
on_selection_changed (GtkSelectionModel *model,
                      guint              position,
                      guint              n_items,
                      gpointer           user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);
  StampContactItem *item = STAMP_CONTACT_ITEM (gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (model)));

  if (item) {
    g_signal_emit (self, signals[CONTACT_SELECTED], 0, self->account, item);
  } else {
    g_signal_emit (self, signals[CONTACT_SELECTED], 0, NULL, 0);
  }
}

static void
on_search_contact (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);
  g_autoptr (GError) error = NULL;
  g_autoslist (EContact) list = NULL;

  list = stamp_account_search_contacts_finish (self->account, E_BOOK_CLIENT (self->client), res, &error);

  if (list && g_slist_length (list) > 0) {
    stamp_contact_list_load (self, self->client, self->account);
    gtk_filter_changed (self->filter, GTK_FILTER_CHANGE_DIFFERENT);
  }

  g_clear_handle_id (&self->progress_handle, g_source_remove);
  gtk_widget_set_visible (self->progress, FALSE);
}

static gboolean
progress_indicator (gpointer user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);

  g_usleep (0.05 * G_USEC_PER_SEC);
  gtk_progress_bar_pulse (GTK_PROGRESS_BAR (self->progress));

  return G_SOURCE_CONTINUE;
}

static void
on_contact_search_entry_changed (GtkWidget *search_entry,
                                 gpointer   user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);
  const gchar *sexp = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));

  /* Searching contact */
  if (self->account && sexp && strlen (sexp) > 0) {
    if (!self->progress_handle) {
      gtk_widget_set_visible (self->progress, TRUE);
      self->progress_handle = g_idle_add (progress_indicator, self);
    }

    stamp_account_search_contacts (self->account, E_BOOK_CLIENT (self->client), sexp, NULL, on_search_contact, self);
  } else {
    g_clear_handle_id (&self->progress_handle, g_source_remove);
    gtk_widget_set_visible (self->progress, FALSE);
  }

  gtk_filter_changed (self->filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static void
on_items_changed (GListModel *model,
                  guint       position,
                  guint       removed,
                  guint       added,
                  gpointer    user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);
  guint n_items = g_list_model_get_n_items (model);

  if (n_items == 0) {
    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "notfound");
  } else {
    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "content");
  }
}

static void
stamp_contact_list_get_property (GObject    *object,
                                 guint       property_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  StampContactList *self = STAMP_CONTACT_LIST (object);

  switch ((StampContactListProps)property_id) {
    case PROP_SORT_MODE:
      g_value_set_enum (value, self->sort_mode);
      break;
  }
}
static void
stamp_contact_list_set_property (GObject      *object,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  StampContactList *self = STAMP_CONTACT_LIST (object);

  switch ((StampContactListProps)property_id) {
    case PROP_SORT_MODE:
      self->sort_mode = g_value_get_enum (value);
      break;
  }
}

static void
stamp_contact_list_class_init (StampContactListClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = stamp_contact_list_get_property;
  object_class->set_property = stamp_contact_list_set_property;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/contact/contact-list/stamp-contact-list.ui");

  gtk_widget_class_bind_template_child (widget_class, StampContactList, sort_list_model);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, filter_list_model);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, search_bar);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, search_entry);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, window_title);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, selection);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, progress);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, stack);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, sort_button);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, sidebar_button);
  gtk_widget_class_bind_template_child (widget_class, StampContactList, normal_headerbar);


  gtk_widget_class_bind_template_callback (widget_class, on_setup_list_item);
  gtk_widget_class_bind_template_callback (widget_class, on_bind_list_item);
  gtk_widget_class_bind_template_callback (widget_class, on_selection_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_contact_search_entry_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_items_changed);

  signals[CONTACT_SELECTED] = g_signal_new ("contact-selected", G_OBJECT_CLASS_TYPE (klass),
                                            G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE,
                                            2, G_TYPE_OBJECT, G_TYPE_POINTER);

  properties[PROP_SORT_MODE] = g_param_spec_enum ("sort-mode",
                                                  NULL,
                                                  NULL,
                                                  STAMP_TYPE_SORT_MODE,
                                                  SORT_MODE_GIVEN_NAME,
                                                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);
}


static void
on_sort_activate (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  StampContactList *self = STAMP_CONTACT_LIST (user_data);
  const gchar *value;

  if (!parameter)
    return;

  /* neuen Zustand setzen */
  g_simple_action_set_state (action, parameter);

  value = g_variant_get_string (parameter, NULL);

  if (g_strcmp0 (value, "given-name") == 0)
    self->sort_mode = SORT_MODE_GIVEN_NAME;
  else
    self->sort_mode = SORT_MODE_FAMILY_NAME;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SORT_MODE]);

  gtk_sorter_changed (self->sorter, GTK_SORTER_CHANGE_INVERTED);
}

static void
stamp_contact_list_init (StampContactList *self)
{
  GSimpleAction *sort_action = g_simple_action_new_stateful ("contacts-sort", G_VARIANT_TYPE_STRING, g_variant_new_string ("given-name"));
  GSimpleActionGroup *actions = g_simple_action_group_new ();
  GMenu *menu;
  GMenu *sort_menu;

  gtk_widget_init_template (GTK_WIDGET (self));

  self->list_store = g_list_store_new (STAMP_TYPE_CONTACT_ITEM);

  gtk_sort_list_model_set_model (self->sort_list_model, G_LIST_MODEL (self->list_store));
  self->sorter = GTK_SORTER (gtk_custom_sorter_new (sorter_func, self, NULL));
  gtk_sort_list_model_set_sorter (self->sort_list_model, self->sorter);

  self->filter = GTK_FILTER (gtk_custom_filter_new (filter_func, self, NULL));
  gtk_filter_list_model_set_filter (self->filter_list_model, self->filter);

  gtk_search_bar_connect_entry (GTK_SEARCH_BAR (self->search_bar), GTK_EDITABLE (self->search_entry));

  menu = g_menu_new ();
  sort_menu = g_menu_new ();
  g_menu_append_section (menu, _("Sort Order"), G_MENU_MODEL (sort_menu));
  g_menu_append (sort_menu, _("Given Name"), "contact.contacts-sort::given-name");
  g_menu_append (sort_menu, _("Family Name"), "contact.contacts-sort::family-name");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (self->sort_button), G_MENU_MODEL (menu));

  g_signal_connect_object (sort_action, "activate", G_CALLBACK (on_sort_activate), self, G_CONNECT_DEFAULT);
  g_action_map_add_action (G_ACTION_MAP (actions), G_ACTION (sort_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "contact", G_ACTION_GROUP (actions));
}

GtkWidget *
stamp_contact_list_new (void)
{
  return g_object_new (STAMP_TYPE_CONTACT_LIST, NULL);
}

void
stamp_contact_list_unselect (StampContactList *self)
{
  gtk_selection_model_unselect_all (GTK_SELECTION_MODEL (self->selection));
}

StampAccount *
stamp_contact_list_get_account (StampContactList *self)
{
  return self->account;
}

GtkWidget *
stamp_contact_list_get_sidebar_button (StampContactList *self)
{
  return self->sidebar_button;
}

void
stamp_contact_list_search_contact (StampContactList *self,
                                   const gchar      *mail)
{
  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), mail);
  gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (self->search_bar), TRUE);
}

void
stamp_contact_list_set_show_buttons (StampContactList *self,
                                     gboolean          show)
{
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (self->normal_headerbar), show);
}
