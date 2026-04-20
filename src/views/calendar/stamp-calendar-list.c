/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "stamp-calendar-list.h"

#include <core/gcal-calendar.h>
#include <glib/gi18n.h>

struct _StampCalendarList {
  AdwBin parent_instance;

  GListModel *model;
  GListStore *accounts;
  GHashTable *groups;
  GtkListView *list;
};

G_DEFINE_FINAL_TYPE (StampCalendarList, stamp_calendar_list, ADW_TYPE_BIN)

static const gchar *
account_name (GListStore *group)
{
  ESource *source = g_object_get_data (G_OBJECT (group), "source");

  return source ? e_source_get_display_name (source) : _("On This Computer");
}

static gint
compare_accounts (gconstpointer a,
                  gconstpointer b,
                  gpointer      user_data)
{
  return g_utf8_collate (account_name (G_LIST_STORE ((gpointer)a)),
                         account_name (G_LIST_STORE ((gpointer)b)));
}

static gint
compare_calendars (gconstpointer a,
                   gconstpointer b,
                   gpointer      user_data)
{
  return g_utf8_collate (gcal_calendar_get_name (GCAL_CALENDAR ((gpointer)a)),
                         gcal_calendar_get_name (GCAL_CALENDAR ((gpointer)b)));
}

static void
sync_accounts (StampCalendarList *self)
{
  g_autoptr (GHashTable) present = g_hash_table_new (g_direct_hash, g_direct_equal);
  GHashTableIter iter;
  gpointer value;

  for (guint i = 0; self->model && i < g_list_model_get_n_items (self->model); i++) {
    g_autoptr (GcalCalendar) calendar = g_list_model_get_item (self->model, i);
    ESource *source = gcal_calendar_get_parent_source (calendar);
    const gchar *uid = source ? e_source_get_uid (source) : "";
    GListStore *group;

    /* Hide the built-in groups by identity, including translated/renamed ones. */
    if (source == NULL || g_str_equal (uid, "contacts-stub") || g_str_equal (uid, "local-stub"))
      continue;

    group = g_hash_table_lookup (self->groups, uid);

    if (group == NULL) {
      group = g_list_store_new (GCAL_TYPE_CALENDAR);
      if (source)
        g_object_set_data_full (G_OBJECT (group), "source", g_object_ref (source), g_object_unref);
      g_hash_table_insert (self->groups, g_strdup (uid), group);
      g_list_store_insert_sorted (self->accounts, group, compare_accounts, NULL);
    }

    if (!g_list_store_find (group, calendar, NULL))
      g_list_store_insert_sorted (group, calendar, compare_calendars, NULL);
    g_hash_table_add (present, calendar);
  }

  g_hash_table_iter_init (&iter, self->groups);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    GListStore *group = value;
    guint position;

    for (guint i = g_list_model_get_n_items (G_LIST_MODEL (group)); i > 0; i--) {
      g_autoptr (GcalCalendar) calendar = g_list_model_get_item (G_LIST_MODEL (group), i - 1);

      if (!g_hash_table_contains (present, calendar))
        g_list_store_remove (group, i - 1);
    }

    if (g_list_model_get_n_items (G_LIST_MODEL (group)) == 0) {
      if (g_list_store_find (self->accounts, group, &position))
        g_list_store_remove (self->accounts, position);
      g_hash_table_iter_remove (&iter);
    }
  }
}

static void
on_calendars_changed (GListModel        *model,
                      guint              position,
                      guint              removed,
                      guint              added,
                      StampCalendarList *self)
{
  sync_accounts (self);
}

static GListModel *
account_children (gpointer item,
                  gpointer user_data)
{
  return G_IS_LIST_STORE (item) ? G_LIST_MODEL (g_object_ref (item)) : NULL;
}

static gboolean
color_to_paintable (GBinding     *binding,
                    const GValue *from,
                    GValue       *to,
                    gpointer      user_data)
{
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  graphene_rect_t rect = GRAPHENE_RECT_INIT (0, 0, 16, 16);
  graphene_size_t size = GRAPHENE_SIZE_INIT (16, 16);
  GskRoundedRect rounded;

  gsk_rounded_rect_init_from_rect (&rounded, &rect, 8);
  gtk_snapshot_push_rounded_clip (snapshot, &rounded);
  gtk_snapshot_append_color (snapshot, g_value_get_boxed (from), &rect);
  gtk_snapshot_pop (snapshot);
  g_value_take_object (to, gtk_snapshot_free_to_paintable (snapshot, &size));
  return TRUE;
}

static void
clear_binding (gpointer data)
{
  g_binding_unbind (data);
  g_object_unref (data);
}

static void
on_setup_row (GtkSignalListItemFactory *factory,
              GtkListItem              *item,
              gpointer                  user_data)
{
  GtkWidget *expander = gtk_tree_expander_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *icon = gtk_image_new ();
  GtkWidget *label = gtk_label_new (NULL);
  GtkWidget *checkmark = gtk_image_new_from_icon_name ("checkmark-small-symbolic");

  gtk_widget_add_css_class (box, "calendar-row");
  gtk_widget_set_valign (checkmark, GTK_ALIGN_CENTER);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), label);
  gtk_box_append (GTK_BOX (box), checkmark);
  gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), box);
  gtk_tree_expander_set_indent_for_depth (GTK_TREE_EXPANDER (expander), TRUE);
  gtk_tree_expander_set_indent_for_icon (GTK_TREE_EXPANDER (expander), TRUE);
  gtk_list_item_set_child (item, expander);
  gtk_list_item_set_selectable (item, FALSE);
  g_object_set_data (G_OBJECT (item), "icon", icon);
  g_object_set_data (G_OBJECT (item), "label", label);
  g_object_set_data (G_OBJECT (item), "checkmark", checkmark);
}

static void
on_bind_row (GtkSignalListItemFactory *factory,
             GtkListItem              *item,
             gpointer                  user_data)
{
  GtkTreeListRow *row = gtk_list_item_get_item (item);
  g_autoptr (GObject) object = gtk_tree_list_row_get_item (row);
  GtkWidget *expander = gtk_list_item_get_child (item);
  GtkWidget *row_widget = gtk_widget_get_parent (expander);
  GtkWidget *icon = g_object_get_data (G_OBJECT (item), "icon");
  GtkWidget *label = g_object_get_data (G_OBJECT (item), "label");
  GtkWidget *checkmark = g_object_get_data (G_OBJECT (item), "checkmark");
  gboolean account = G_IS_LIST_STORE (object);
  GBinding *binding;

  gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), row);
  gtk_widget_remove_css_class (row_widget, "tree-header");
  gtk_widget_remove_css_class (row_widget, "tree-child");
  gtk_widget_add_css_class (row_widget, account ? "tree-header" : "tree-child");
  gtk_widget_set_visible (icon, !account);
  gtk_widget_set_visible (checkmark, FALSE);

  if (account) {
    ESource *source = g_object_get_data (object, "source");

    gtk_label_set_text (GTK_LABEL (label), account_name (G_LIST_STORE (object)));
    if (source) {
      binding = g_object_bind_property (source, "display-name", label, "label", G_BINDING_SYNC_CREATE);
      g_object_set_data_full (G_OBJECT (item), "name-binding", g_object_ref (binding), clear_binding);
    }
  } else {
    binding = g_object_bind_property (object, "name", label, "label", G_BINDING_SYNC_CREATE);
    g_object_set_data_full (G_OBJECT (item), "name-binding", g_object_ref (binding), clear_binding);
    binding = g_object_bind_property (object, "visible", checkmark, "visible", G_BINDING_SYNC_CREATE);
    g_object_set_data_full (G_OBJECT (item), "visible-binding", g_object_ref (binding), clear_binding);
    binding = g_object_bind_property_full (object, "color", icon, "paintable",
                                           G_BINDING_SYNC_CREATE, color_to_paintable, NULL, NULL, NULL);
    g_object_set_data_full (G_OBJECT (item), "color-binding", g_object_ref (binding), clear_binding);
  }
}

static void
on_unbind_row (GtkSignalListItemFactory *factory,
               GtkListItem              *item,
               gpointer                  user_data)
{
  g_object_set_data (G_OBJECT (item), "name-binding", NULL);
  g_object_set_data (G_OBJECT (item), "visible-binding", NULL);
  g_object_set_data (G_OBJECT (item), "color-binding", NULL);
  gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (gtk_list_item_get_child (item)), NULL);
}

static void
on_activate_row (GtkListView       *list,
                 guint              position,
                 StampCalendarList *self)
{
  g_autoptr (GtkTreeListRow) row = g_list_model_get_item (G_LIST_MODEL (gtk_list_view_get_model (list)), position);
  g_autoptr (GObject) object = gtk_tree_list_row_get_item (row);

  if (G_IS_LIST_STORE (object))
    gtk_tree_list_row_set_expanded (row, !gtk_tree_list_row_get_expanded (row));
  else
    gcal_calendar_set_visible (GCAL_CALENDAR (object), !gcal_calendar_get_visible (GCAL_CALENDAR (object)));
}

static void
stamp_calendar_list_dispose (GObject *object)
{
  StampCalendarList *self = STAMP_CALENDAR_LIST (object);

  if (self->model)
    g_signal_handlers_disconnect_by_data (self->model, self);
  adw_bin_set_child (ADW_BIN (self), NULL);
  g_clear_object (&self->model);
  g_clear_object (&self->accounts);
  g_clear_pointer (&self->groups, g_hash_table_unref);

  G_OBJECT_CLASS (stamp_calendar_list_parent_class)->dispose (object);
}

static void
stamp_calendar_list_class_init (StampCalendarListClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = stamp_calendar_list_dispose;
}

static void
stamp_calendar_list_init (StampCalendarList *self)
{
  GtkTreeListModel *tree;
  GtkListItemFactory *factory;

  self->accounts = g_list_store_new (G_TYPE_LIST_STORE);
  self->groups = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  tree = gtk_tree_list_model_new (G_LIST_MODEL (g_object_ref (self->accounts)), FALSE, TRUE,
                                  account_children, NULL, NULL);
  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (on_setup_row), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (on_bind_row), NULL);
  g_signal_connect (factory, "unbind", G_CALLBACK (on_unbind_row), NULL);
  self->list = GTK_LIST_VIEW (gtk_list_view_new (GTK_SELECTION_MODEL (gtk_no_selection_new (G_LIST_MODEL (tree))), factory));
  gtk_list_view_set_single_click_activate (self->list, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "navigation-sidebar");
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "listview");
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "calendar-sidebar");
  g_signal_connect_object (self->list, "activate", G_CALLBACK (on_activate_row), self, G_CONNECT_DEFAULT);
  adw_bin_set_child (ADW_BIN (self), GTK_WIDGET (self->list));
}

void
stamp_calendar_list_set_model (StampCalendarList *self,
                               GListModel        *model)
{
  g_return_if_fail (STAMP_IS_CALENDAR_LIST (self));
  g_return_if_fail (model == NULL || G_IS_LIST_MODEL (model));

  if (self->model == model)
    return;

  if (self->model)
    g_signal_handlers_disconnect_by_data (self->model, self);
  g_set_object (&self->model, model);
  g_list_store_remove_all (self->accounts);
  g_hash_table_remove_all (self->groups);
  if (model)
    g_signal_connect_object (model, "items-changed", G_CALLBACK (on_calendars_changed), self, G_CONNECT_DEFAULT);
  sync_accounts (self);
}
