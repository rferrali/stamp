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

#include "stamp-contact-completion.h"

#include <ctype.h>

#include <libebook/libebook.h>
#include <libpsl.h>

#include "stamp-account.h"
#include "stamp-tag.h"

struct _StampContactCompletion {
  AdwBin parent_instance;

  GtkWidget *wrap_box;
  GtkWidget *entry;
  GtkWidget *popover;
  GtkWidget *view;

  GCancellable *cancellable;
  guint search_timeout_id;
  GtkSingleSelection *model;
  GListStore *store;
  GtkFilter *filter;

  GList *receivers;
  gboolean has_entries;
  StampAccount *account;
  gboolean block;
};

G_DEFINE_FINAL_TYPE (StampContactCompletion, stamp_contact_completion, ADW_TYPE_BIN);

#define PAGE_STEP 20

typedef enum {
  PROP_HAS_ENTRIES = 1,
} StampContactCompletionProps;

static GParamSpec *properties[PROP_HAS_ENTRIES + 1];

static void
on_tag_destroy (GtkWidget *obj,
                gpointer   user_data)
{
  StampTag *tag = STAMP_TAG (obj);
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (user_data);

  self->receivers = g_list_remove (self->receivers, tag);
  self->has_entries = g_list_length (self->receivers) > 0;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_HAS_ENTRIES]);

  self->has_entries = g_list_length (self->receivers) > 0;
}

static void
on_activate (GtkListView *view,
             guint        position,
             gpointer     user_data)
{
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (user_data);
  g_autoptr (EContact) contact = g_list_model_get_item (G_LIST_MODEL (self->model), position);
  GList *emails = e_contact_get (contact, E_CONTACT_EMAIL);
  GtkWidget *tag = stamp_tag_new (self->account);
  const gchar *name = e_contact_get_const (contact, E_CONTACT_FULL_NAME);

  gtk_editable_set_text (GTK_EDITABLE (self->entry), "");

  stamp_tag_set_label (STAMP_TAG (tag), name ? name : (const gchar *)emails->data);
  stamp_tag_set_mail (STAMP_TAG (tag), emails->data);

  stamp_contact_completion_add_tag (self, STAMP_TAG (tag));
  g_list_free_full (emails, g_free);

  gtk_popover_popdown (GTK_POPOVER (self->popover));
}

static gboolean
is_valid_email (const gchar *str)
{
  static GRegex *regex = NULL;
  gboolean ret;

  if (!regex)
    regex = g_regex_new (
      "^[a-zA-Z0-9._%+\\-]+@[a-zA-Z0-9.\\-]+\\.[a-zA-Z]{2,}$",
      0, 0, NULL);

  ret = g_regex_match (regex, str, 0, NULL);
  if (ret) {
    const psl_ctx_t *psl = psl_builtin ();
    return psl_registrable_domain (psl, strchr (str, '@') + 1) != NULL;
  }

  return FALSE;
}

static gboolean
convert_to_tag (StampContactCompletion *self,
                const gchar            *text,
                gboolean                focus_leave)
{
  g_autofree char *stripped = g_strstrip (g_strdup (text));

  if (is_valid_email (stripped)) {
    GtkWidget *tag = stamp_tag_new (self->account);

    stamp_tag_set_label (STAMP_TAG (tag), stripped);
    stamp_tag_set_mail (STAMP_TAG (tag), stripped);

    stamp_contact_completion_add_tag (self, STAMP_TAG (tag));

    return TRUE;
  }

  return FALSE;
}

static void
on_focus_leave (GtkEventControllerFocus *controller,
                gpointer                 user_data)
{
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (user_data);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  GtkWidget *focus_widget;

  if (!root)
    return;

  focus_widget = gtk_root_get_focus (root);

  if (focus_widget && gtk_widget_has_focus (self->entry) && gtk_widget_is_ancestor (focus_widget, GTK_WIDGET (self->entry)))
    return;

  if (convert_to_tag (self, gtk_editable_get_text (GTK_EDITABLE (self->entry)), TRUE))
    gtk_editable_set_text (GTK_EDITABLE (self->entry), "");

  gtk_popover_popdown (GTK_POPOVER (self->popover));
}

static void
on_items_changed (GListModel *model,
                  guint       position,
                  guint       removed,
                  guint       added,
                  gpointer    user_data)
{
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (user_data);
  GtkPopover *pop = GTK_POPOVER (self->popover);
  gint items = g_list_model_get_n_items (G_LIST_MODEL (self->model));
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (self->entry));

  if (!self->popover || !gtk_widget_get_root (GTK_WIDGET (self)))
    return;
  if (items > 0 && text && strlen (text) > 0) {
    GdkRectangle rect = { 0, 0, 1, gtk_widget_get_height (GTK_WIDGET (self)) };

    gtk_popover_set_position (GTK_POPOVER (pop), GTK_POS_BOTTOM);

    gtk_widget_set_halign (GTK_WIDGET (pop), GTK_ALIGN_START);
    gtk_popover_set_pointing_to (GTK_POPOVER (pop), &rect);
    gtk_popover_popup (pop);
  } else {
    gtk_popover_popdown (pop);
  }
}

static gboolean
on_key_pressed (GtkEventControllerKey  *controller,
                guint                   keyval,
                guint                   keycode,
                GdkModifierType         state,
                StampContactCompletion *self)
{
  gint selected, matches;

  if (state & (GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_CONTROL_MASK))
    return FALSE;

  if (keyval == GDK_KEY_Escape) {
    gtk_popover_popdown (GTK_POPOVER (self->popover));
    return TRUE;
  }

  if (keyval == GDK_KEY_BackSpace) {
    const gchar *text = gtk_editable_get_text (GTK_EDITABLE (self->entry));
    g_autofree char *mail = NULL;
    StampTag *tag;

    if (*text)
      return FALSE;

    if (!self->receivers)
      return TRUE;

    tag = g_list_last (self->receivers)->data;
    mail = g_strdup (stamp_tag_get_mail (tag));

    gtk_widget_unparent (GTK_WIDGET (tag));
    gtk_editable_set_text (GTK_EDITABLE (self->entry), mail);
    gtk_editable_set_position (GTK_EDITABLE (self->entry), -1);
    return TRUE;
  }

  if (keyval != GDK_KEY_Up && keyval != GDK_KEY_KP_Up &&
      keyval != GDK_KEY_Down && keyval != GDK_KEY_KP_Down &&
      keyval != GDK_KEY_Page_Up && keyval != GDK_KEY_KP_Page_Up &&
      keyval != GDK_KEY_Page_Down && keyval != GDK_KEY_KP_Page_Down) {
    return FALSE;
  }

  if (!gtk_widget_get_visible (self->popover))
    return FALSE;

  matches = g_list_model_get_n_items (G_LIST_MODEL (self->model));
  selected = gtk_single_selection_get_selected (self->model);

  if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
    if (selected <= 0)
      selected = matches - 1;
    else
      selected--;
  } else if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
    if (selected < matches - 1)
      selected++;
    else if (selected == matches - 1)
      selected = 0;
    else
      selected = -1;
  } else if (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_KP_Page_Up) {
    if (selected == -1)
      selected = matches - 1;
    else if (selected == 0)
      selected = -1;
    else if (selected < PAGE_STEP)
      selected = 0;
    else
      selected -= PAGE_STEP;
  } else if (keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_KP_Page_Down) {
    if (selected == -1)
      selected = 0;
    else if (selected == matches - 1)
      selected = -1;
    else if (selected + PAGE_STEP > matches - 1)
      selected = matches - 1;
    else
      selected += PAGE_STEP;
  }

  if (selected < 0) {
    /* Unselect and restore text */
    gtk_single_selection_set_selected (self->model, GTK_INVALID_LIST_POSITION);
  } else if (selected < matches) {
    gtk_single_selection_set_selected (self->model, selected);
    gtk_list_view_scroll_to (GTK_LIST_VIEW (self->view), selected, GTK_LIST_SCROLL_NONE, NULL);
    /* set_selected_suggestion_as_url (self); */
  }

  return TRUE;
}

static void
on_entry_activate (GtkWidget *widget,
                   gpointer   user_data)
{
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (user_data);

  if (gtk_widget_get_visible (self->popover)) {
    guint position = gtk_single_selection_get_selected (self->model);

    if (position != GTK_INVALID_LIST_POSITION) {
      on_activate (NULL, position, self);
      return;
    }
  }

  if (convert_to_tag (self, gtk_editable_get_text (GTK_EDITABLE (self->entry)), TRUE))
    gtk_editable_set_text (GTK_EDITABLE (self->entry), "");

  gtk_popover_popdown (GTK_POPOVER (self->popover));
}

static void
cancel_search (StampContactCompletion *self)
{
  g_clear_handle_id (&self->search_timeout_id, g_source_remove);
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
}

typedef struct {
  GWeakRef owner;
  GCancellable *cancellable;
  GHashTable *emails;
  GPtrArray *contacts;
  guint pending;
  guint published;
} CompletionSearch;

static void
completion_search_add (CompletionSearch *search,
                       GSList           *contacts)
{
  for (GSList *iter = contacts; iter; iter = iter->next) {
    EContact *contact = iter->data;
    GList *emails = e_contact_get (contact, E_CONTACT_EMAIL);

    for (GList *email = emails; email; email = email->next) {
      const gchar *address = email->data;
      g_autofree gchar *key = NULL;
      EContact *copy;
      GList single = { (gpointer)address, NULL, NULL };

      if (!address || !*address)
        continue;

      key = g_utf8_casefold (address, -1);
      if (g_hash_table_contains (search->emails, key))
        continue;

      g_hash_table_add (search->emails, g_steal_pointer (&key));

      copy = e_contact_duplicate (contact);
      e_contact_set (copy, E_CONTACT_EMAIL, &single);
      g_ptr_array_add (search->contacts, copy);
    }

    g_list_free_full (emails, g_free);
  }
}

static void
completion_search_done (CompletionSearch *search)
{
  g_autoptr (StampContactCompletion) self = NULL;

  self = g_weak_ref_get (&search->owner);
  if (self && self->cancellable == search->cancellable && !g_cancellable_is_cancelled (search->cancellable)) {
    if (search->contacts->len > search->published ||
        (search->published == 0 && search->pending == 1)) {
      g_list_store_splice (self->store, 0, g_list_model_get_n_items (G_LIST_MODEL (self->store)), search->contacts->pdata, search->contacts->len);
      gtk_filter_changed (self->filter, GTK_FILTER_CHANGE_DIFFERENT);
      search->published = search->contacts->len;
    }
  }

  if (--search->pending != 0)
    return;

  g_weak_ref_clear (&search->owner);
  g_object_unref (search->cancellable);
  g_hash_table_unref (search->emails);
  g_ptr_array_unref (search->contacts);
  g_free (search);
}

static void
on_search_contacts (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  CompletionSearch *search = user_data;
  g_autoptr (GError) error = NULL;
  g_autoslist (EContact) contacts = NULL;

  contacts = stamp_account_search_contacts_finish (NULL, E_BOOK_CLIENT (source), res, &error);
  if (!error) {
    if (!g_cancellable_is_cancelled (search->cancellable))
      completion_search_add (search, contacts);
  } else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    ESource *book = e_client_get_source (E_CLIENT (source));

    g_warning ("Could not search address book '%s': %s", e_source_get_display_name (book), error->message);
  }

  completion_search_done (search);
}

static void
on_search_conversations (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  CompletionSearch *search = user_data;
  g_autoptr (GError) error = NULL;
  g_autoslist (EContact) contacts = stamp_account_search_conversations_finish (STAMP_ACCOUNT (source), res, &error);

  if (!g_cancellable_is_cancelled (search->cancellable) && !error)
    completion_search_add (search, contacts);

  completion_search_done (search);
}

static gboolean
start_completion_search (gpointer user_data)
{
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (user_data);
  CompletionSearch *search;
  g_autoptr (GHashTable) seen_books = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *books;
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (self->entry));

  self->search_timeout_id = 0;
  if (!self->account || !*text)
    return G_SOURCE_REMOVE;

  self->cancellable = g_cancellable_new ();

  search = g_new0 (CompletionSearch, 1);
  g_weak_ref_init (&search->owner, self);
  search->cancellable = g_object_ref (self->cancellable);
  search->emails = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  search->contacts = g_ptr_array_new_with_free_func (g_object_unref);
  search->pending = 1;

  books = stamp_account_get_books (self->account);
  for (guint i = 0; books && i < books->len; i++) {
    StampContactsService *service = g_ptr_array_index (books, i);
    EBookClient *client = stamp_contacts_service_get_client (service);
    ESource *source;
    const gchar *uid;

    if (!stamp_contacts_service_get_enabled (service) || !client)
      continue;

    source = e_client_get_source (E_CLIENT (client));
    uid = e_source_get_uid (source);

    if (!e_source_get_enabled (source) || g_hash_table_contains (seen_books, uid))
      continue;

    g_hash_table_add (seen_books, g_strdup (uid));
    search->pending++;

    stamp_account_search_contacts (self->account, client, text, self->cancellable, on_search_contacts, search);
  }

  if (search->pending == 1) {
    search->pending++;
    stamp_account_search_conversations (self->account, text, self->cancellable, on_search_conversations, search);
  }

  completion_search_done (search);
  return G_SOURCE_REMOVE;
}

static void
on_changed (GtkEditable *ed,
            gpointer     user_data)
{
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (user_data);
  g_autofree char *text = NULL;

  if (self->block)
    return;

  text = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->entry)));
  self->block = TRUE;
  cancel_search (self);

  if (text) {
    g_auto (GStrv) split = g_strsplit (g_strstrip (text), ",", -1);
    g_autoptr (GString) new_string = g_string_new (NULL);
    gint n_parts = g_strv_length (split);
    gboolean converted = FALSE;

    for (gint idx = 0; idx < n_parts; idx++) {
      gchar *part = g_strstrip (split[idx]);
      gboolean is_last = (idx == n_parts - 1);

      if (*part && is_valid_email (part) && !is_last) {
        convert_to_tag (self, part, FALSE);
        converted = TRUE;
      } else {
        if (new_string->len > 0)
          g_string_append (new_string, ", ");
        g_string_append (new_string, part);
      }
    }

    if (converted) {
      gtk_editable_set_text (GTK_EDITABLE (self->entry), new_string->str);
      gtk_editable_set_position (GTK_EDITABLE (self->entry), -1);
    }

    if (new_string->len == 0 || !self->account)
      g_list_store_remove_all (self->store);
    gtk_filter_changed (self->filter, GTK_FILTER_CHANGE_DIFFERENT);

    if (new_string->len > 0 && self->account)
      self->search_timeout_id = g_timeout_add (300, start_completion_search, self);
  }

  self->block = FALSE;
}

static void
stamp_contact_completion_set_property (GObject      *object,
                                       guint         property_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (object);

  switch ((StampContactCompletionProps)property_id) {
    case PROP_HAS_ENTRIES:
      self->has_entries = g_value_get_boolean (value);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_HAS_ENTRIES]);
      break;
  }
}

static void
stamp_contact_completion_get_property (GObject    *object,
                                       guint       property_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (object);

  switch ((StampContactCompletionProps)property_id) {
    case PROP_HAS_ENTRIES:
      g_value_set_boolean (value, self->has_entries);
      break;
  }
}


static void
stamp_contact_completion_dispose (GObject *object)
{
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (object);

  self->block = TRUE;

  cancel_search (self);

  if (self->popover) {
    gtk_widget_unparent (self->popover);
    self->popover = NULL;
  }

  g_clear_object (&self->model);
  g_clear_object (&self->filter);
  g_clear_object (&self->store);
  g_clear_object (&self->account);
  g_clear_pointer (&self->receivers, g_list_free);

  adw_bin_set_child (ADW_BIN (self), NULL);
  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_CONTACT_COMPLETION);

  G_OBJECT_CLASS (stamp_contact_completion_parent_class)->dispose (object);
}

static void
stamp_contact_completion_class_init (StampContactCompletionClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/mail/composer/stamp-contact-completion.ui");

  gtk_widget_class_bind_template_child (widget_class, StampContactCompletion, wrap_box);
  gtk_widget_class_bind_template_child (widget_class, StampContactCompletion, entry);

  gtk_widget_class_bind_template_callback (widget_class, on_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_entry_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_focus_leave);
  gtk_widget_class_bind_template_callback (widget_class, on_key_pressed);

  object_class->dispose = stamp_contact_completion_dispose;
  object_class->set_property = stamp_contact_completion_set_property;
  object_class->get_property = stamp_contact_completion_get_property;

  properties[PROP_HAS_ENTRIES] =
    g_param_spec_boolean ("has-entries",
                          NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);
}

static gint
match (gpointer item,
       gpointer user_data)
{
  EContact *contact = E_CONTACT (item);
  StampContactCompletion *self = STAMP_CONTACT_COMPLETION (user_data);
  const gchar *name = e_contact_get_const (contact, E_CONTACT_FULL_NAME);
  const gchar *text = gtk_editable_get_text (GTK_EDITABLE (self->entry));
  g_autofree char *lower_text = NULL;
  g_autofree char *lower_contact = NULL;
  g_autoptr (GString) contact_text = g_string_new (name ? name : "");
  g_auto (GStrv) words = NULL;
  GList *emails;

  if (!text)
    return 0;

  lower_text = g_utf8_casefold (text, -1);
  emails = e_contact_get (contact, E_CONTACT_EMAIL);
  for (GList *iter = emails; iter; iter = iter->next)
    g_string_append_printf (contact_text, "\n%s", (const gchar *)iter->data);
  g_list_free_full (emails, g_free);

  lower_contact = g_utf8_casefold (contact_text->str, -1);
  words = g_strsplit_set (lower_text, " \t\r\n", -1);
  for (guint i = 0; words[i]; i++) {
    if (*words[i] && !strstr (lower_contact, words[i]))
      return 0;
  }

  return 1;
}

static gint
sort (gconstpointer a,
      gconstpointer b,
      gpointer      user_data)
{
  EContact *contact_a = (EContact *)a;
  EContact *contact_b = (EContact *)b;
  const gchar *name_a = e_contact_get_const (contact_a, E_CONTACT_FULL_NAME);
  const gchar *name_b = e_contact_get_const (contact_b, E_CONTACT_FULL_NAME);

  return g_strcmp0 (name_a, name_b);
}


static void
on_setup (GtkSignalListItemFactory *f,
          GtkListItem              *item,
          gpointer                  data)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *avatar = adw_avatar_new (32, "", TRUE);
  GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *title = gtk_label_new (NULL);
  GtkWidget *subtitle = gtk_label_new (NULL);

  gtk_widget_set_hexpand (row, TRUE);
  gtk_widget_set_margin_start (row, 12);
  gtk_widget_set_margin_end (row, 12);
  gtk_widget_set_margin_top (row, 8);
  gtk_widget_set_margin_bottom (row, 8);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (labels, TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_xalign (GTK_LABEL (subtitle), 0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize (GTK_LABEL (subtitle), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (subtitle, "dim-label");
  gtk_widget_add_css_class (subtitle, "caption");
  gtk_box_append (GTK_BOX (labels), title);
  gtk_box_append (GTK_BOX (labels), subtitle);
  gtk_box_append (GTK_BOX (row), avatar);
  gtk_box_append (GTK_BOX (row), labels);
  g_object_set_data (G_OBJECT (row), "avatar", avatar);
  g_object_set_data (G_OBJECT (row), "title", title);
  g_object_set_data (G_OBJECT (row), "subtitle", subtitle);
  gtk_list_item_set_child (item, row);
}

static void
set_contact_photo (AdwAvatar *avatar,
                   EContact  *contact)
{
  EContactPhoto *photo = e_contact_get (contact, E_CONTACT_PHOTO);
  g_autoptr (GdkTexture) texture = NULL;

  /* Binding suggestions must not start another search through every book. */
  if (photo) {
    if (photo->type == E_CONTACT_PHOTO_TYPE_INLINED) {
      g_autoptr (GBytes) bytes = g_bytes_new (photo->data.inlined.data, photo->data.inlined.length);

      texture = gdk_texture_new_from_bytes (bytes, NULL);
    } else if (photo->type == E_CONTACT_PHOTO_TYPE_URI && photo->data.uri &&
               g_str_has_prefix (photo->data.uri, "file://")) {
      g_autoptr (GFile) file = g_file_new_for_uri (photo->data.uri);

      texture = gdk_texture_new_from_file (file, NULL);
    }
    e_contact_photo_free (photo);
  }

  adw_avatar_set_custom_image (avatar, texture ? GDK_PAINTABLE (texture) : NULL);
}

static void
on_bind (GtkSignalListItemFactory *f,
         GtkListItem              *item,
         gpointer                  user_data)
{
  EContact *contact = gtk_list_item_get_item (item);
  GtkWidget *row = gtk_list_item_get_child (item);
  GtkWidget *avatar = g_object_get_data (G_OBJECT (row), "avatar");
  const gchar *name = e_contact_get_const (contact, E_CONTACT_FULL_NAME);
  GList *emails = e_contact_get (contact, E_CONTACT_EMAIL);

  if (!emails)
    return;

  adw_avatar_set_text (ADW_AVATAR (avatar), name ? name : (const gchar *)emails->data);
  set_contact_photo (ADW_AVATAR (avatar), contact);

  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (row), "title")),
                      name ? name : (const gchar *)emails->data);
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (row), "subtitle")), emails->data);

  g_list_free_full (emails, g_free);
}

static void
stamp_contact_completion_init (StampContactCompletion *self)
{
  GtkFilterListModel *filtered;
  GtkWidget *scrolled_window;
  GtkSorter *sorter;
  GtkSortListModel *sorted;
  GtkListItemFactory *factory;

  gtk_widget_init_template (GTK_WIDGET (self));

  self->store = g_list_store_new (E_TYPE_CONTACT);

  self->filter = GTK_FILTER (gtk_custom_filter_new (match, self, NULL));
  filtered = gtk_filter_list_model_new (G_LIST_MODEL (g_object_ref (self->store)), GTK_FILTER (g_object_ref (self->filter)));

  sorter = GTK_SORTER (gtk_custom_sorter_new (sort, NULL, NULL));
  sorted = gtk_sort_list_model_new (G_LIST_MODEL (filtered), GTK_SORTER (sorter));

  self->popover = gtk_popover_new ();
  gtk_widget_set_hexpand (self->popover, TRUE);
  gtk_widget_add_css_class (self->popover, "menu");
  gtk_widget_add_css_class (self->popover, "contact-completion-popover");
  gtk_widget_set_parent (self->popover, GTK_WIDGET (self));
  gtk_widget_set_can_focus (self->popover, FALSE);
  gtk_popover_set_has_arrow (GTK_POPOVER (self->popover), FALSE);
  gtk_popover_set_autohide (GTK_POPOVER (self->popover), FALSE);

  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect_object (factory, "setup", G_CALLBACK (on_setup), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (factory, "bind", G_CALLBACK (on_bind), self, G_CONNECT_DEFAULT);

  scrolled_window = gtk_scrolled_window_new ();
  gtk_widget_set_hexpand (scrolled_window, TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled_window), 400);
  gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (scrolled_window), TRUE);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled_window), TRUE);
  self->model = gtk_single_selection_new (G_LIST_MODEL (sorted));
  g_signal_connect_object (self->model, "items-changed", G_CALLBACK (on_items_changed), self, G_CONNECT_DEFAULT);
  self->view = gtk_list_view_new (GTK_SELECTION_MODEL (g_object_ref (self->model)), factory);
  gtk_widget_set_size_request (self->view, 300, -1);
  gtk_scrolled_window_set_policy (
    GTK_SCROLLED_WINDOW (scrolled_window),
    GTK_POLICY_NEVER,
    GTK_POLICY_AUTOMATIC
    );
  gtk_widget_set_hexpand (self->view, TRUE);
  gtk_list_view_set_single_click_activate (GTK_LIST_VIEW (self->view), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled_window), self->view);
  gtk_popover_set_child (GTK_POPOVER (self->popover), scrolled_window);

  g_signal_connect_object (self->view, "activate", G_CALLBACK (on_activate), self, G_CONNECT_DEFAULT);
}

GtkWidget *
stamp_contact_completion_new (void)
{
  return g_object_new (STAMP_TYPE_CONTACT_COMPLETION, NULL);
}

GList *
stamp_contact_completion_get_tags (StampContactCompletion *self)
{
  return self->receivers;
}

void
stamp_contact_completion_add_tag (StampContactCompletion *self,
                                  StampTag               *tag)
{
  g_signal_connect_object (tag, "destroy", G_CALLBACK (on_tag_destroy), self, G_CONNECT_DEFAULT);

  adw_wrap_box_append (ADW_WRAP_BOX (self->wrap_box), GTK_WIDGET (tag));
  self->receivers = g_list_append (self->receivers, tag);
  self->has_entries = TRUE;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_HAS_ENTRIES]);

  adw_wrap_box_reorder_child_after (ADW_WRAP_BOX (self->wrap_box), self->entry, GTK_WIDGET (tag));
}

void
stamp_contact_completion_set_account (StampContactCompletion *self,
                                      StampAccount           *account)
{
  if (self->account == account)
    return;

  cancel_search (self);
  g_set_object (&self->account, account);
  g_list_store_remove_all (self->store);

  on_changed (GTK_EDITABLE (self->entry), self);
}

gboolean
stamp_contact_completion_contains_address (StampContactCompletion *self,
                                           const gchar            *mail)
{
  for (GList *iter = self->receivers; iter && iter->data; iter = g_list_next (iter)) {
    StampTag *tag = STAMP_TAG (iter->data);

    if (g_strcmp0 (stamp_tag_get_mail (tag), mail) == 0)
      return TRUE;
  }

  return FALSE;
}
