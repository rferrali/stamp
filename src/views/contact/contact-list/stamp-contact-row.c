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

#include "stamp-contact-row.h"

#include "stamp-account.h"

struct _StampContactRow {
  AdwBin parent_instance;

  GtkWidget *avatar;
  GtkWidget *name;
};

G_DEFINE_FINAL_TYPE (StampContactRow, stamp_contact_row, ADW_TYPE_BIN);

typedef enum {
  PROP_NAME = 1,
} StampContactRowProps;

static GParamSpec *props[PROP_NAME + 1];

static void
stamp_contact_row_dispose (GObject *object)
{
  StampContactRow *self = STAMP_CONTACT_ROW (object);

  gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_CONTACT_ROW);

  G_OBJECT_CLASS (stamp_contact_row_parent_class)->dispose (object);
}

static void
stamp_contact_row_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  StampContactRow *self = STAMP_CONTACT_ROW (object);

  switch ((StampContactRowProps)property_id) {
    case PROP_NAME:
      g_value_set_string (value, gtk_inscription_get_text (GTK_INSCRIPTION (self->name)));
      break;
  }
}
static void
stamp_contact_row_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  StampContactRow *self = STAMP_CONTACT_ROW (object);

  switch ((StampContactRowProps)property_id) {
    case PROP_NAME:
      gtk_inscription_set_text (GTK_INSCRIPTION (self->name), g_value_get_string (value));
      break;
  }
}

void
stamp_contact_row_class_init (StampContactRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = stamp_contact_row_dispose;
  object_class->get_property = stamp_contact_row_get_property;
  object_class->set_property = stamp_contact_row_set_property;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/contact/contact-list/stamp-contact-row.ui");

  gtk_widget_class_bind_template_child (widget_class, StampContactRow, name);
  gtk_widget_class_bind_template_child (widget_class, StampContactRow, avatar);

  props[PROP_NAME] = g_param_spec_string ("name",
                                          NULL,
                                          NULL,
                                          "",
                                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (props), props);
}

void
stamp_contact_row_init (StampContactRow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

GtkWidget *
stamp_contact_row_new (void)
{
  return g_object_new (STAMP_TYPE_CONTACT_ROW, NULL);
}

static gboolean
transfer_sort_mode_to (GBinding     *binding,
                       const GValue *from_value,
                       GValue       *to_value,
                       gpointer      user_data)
{
  StampSortMode sort_mode = g_value_get_enum (from_value);
  StampContactItem *item = STAMP_CONTACT_ITEM (user_data);
  const gchar *given_name = stamp_contact_item_get_given_name (item);
  const gchar *family_name = stamp_contact_item_get_family_name (item);
  g_autoptr (GString) tmp = g_string_new (NULL);

  if (sort_mode == SORT_MODE_GIVEN_NAME) {
    if (given_name && strlen (given_name) > 0) {
      g_string_append (tmp, given_name);
    }

    if (given_name && strlen (given_name) > 0 && family_name && strlen (family_name) > 0) {
      g_string_append (tmp, " ");
      g_string_append (tmp, family_name);
    } else if (family_name && strlen (family_name) > 0) {
      g_string_append (tmp, family_name);
    }

    if (!given_name && !family_name)
      g_string_append (tmp, stamp_contact_item_get_mail (item) ?: "");
  } else {
    if (family_name && strlen (family_name) > 0)
      g_string_append (tmp, family_name);

    if (given_name && strlen (given_name) > 0 && family_name && strlen (family_name) > 0) {
      g_string_append (tmp, ", ");
      g_string_append (tmp, given_name);
    } else if (given_name && strlen (given_name) > 0) {
      g_string_append (tmp, given_name);
    }

    if (!given_name && !family_name)
      g_string_append (tmp, stamp_contact_item_get_mail (item) ?: "");
  }

  /* Fallback to primary phone number */
  if (tmp->len == 0) {
    EContact *contact = stamp_contact_item_get_contact (item);
    g_string_append (tmp, e_contact_get_const (contact, E_CONTACT_PHONE_PRIMARY) ?: "");
  }

  g_value_set_string (to_value, tmp->str);
  return TRUE;
}

void
stamp_contact_row_bind_mail (StampContactRow  *self,
                             StampContactItem *item,
                             StampContactList *list)
{
  const gchar *mail = stamp_contact_item_get_mail (item);
  const gchar *name = stamp_contact_item_get_name (item);
  g_autoptr (GdkTexture) photo = NULL;

  photo = stamp_contact_item_get_photo (item);
  adw_avatar_set_custom_image (ADW_AVATAR (self->avatar), photo ? GDK_PAINTABLE (photo) : NULL);

  if (!name || strlen (name) == 0)
    name = mail;

  g_object_bind_property_full (list, "sort-mode", self->name, "text", G_BINDING_SYNC_CREATE, transfer_sort_mode_to, NULL, item, NULL);
  adw_avatar_set_text (ADW_AVATAR (self->avatar), name);
}
