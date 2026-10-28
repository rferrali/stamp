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

#include "stamp-category.h"

#include <evolution-microsoft365/e-m365-connection.h>

struct _StampCategory {
  GObject parent_instance;

  gchar *id;
  gchar *name;
  gchar *color;
};

typedef enum {
  PROP_ID = 1,
  PROP_NAME,
  PROP_COLOR,
} StampCategoryProps;

static GParamSpec *props[PROP_COLOR + 1];

G_DEFINE_FINAL_TYPE (StampCategory, stamp_category, G_TYPE_OBJECT);

static void
stamp_category_finalize (GObject *object)
{
  StampCategory *self = STAMP_CATEGORY (object);

  g_free (self->id);
  g_free (self->name);
  g_free (self->color);

  G_OBJECT_CLASS (stamp_category_parent_class)->finalize (object);
}

static void
stamp_category_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  StampCategory *self = STAMP_CATEGORY (object);

  switch ((StampCategoryProps)prop_id) {
    case PROP_ID:
      g_value_set_string (value, self->id);
      break;
    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;
    case PROP_COLOR:
      g_value_set_string (value, self->color);
      break;
  }
}

static void
stamp_category_set_property (GObject      *object,
                             guint         property_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  StampCategory *self = STAMP_CATEGORY (object);

  switch ((StampCategoryProps)property_id) {
    case PROP_ID:
      g_set_str (&self->id, g_value_get_string (value));
      break;
    case PROP_NAME:
      g_set_str (&self->name, g_value_get_string (value));
      break;
    case PROP_COLOR:
      g_set_str (&self->color, g_value_get_string (value));
      break;
  }
}

static void
stamp_category_dispose (GObject *object)
{
  StampCategory *self = STAMP_CATEGORY (object);

  g_clear_pointer (&self->id, g_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->color, g_free);

  G_OBJECT_CLASS (stamp_category_parent_class)->dispose (object);
}

static void
stamp_category_class_init (StampCategoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = stamp_category_finalize;
  object_class->get_property = stamp_category_get_property;
  object_class->set_property = stamp_category_set_property;
  object_class->dispose = stamp_category_dispose;

  props[PROP_ID] = g_param_spec_string ("id", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_NAME] = g_param_spec_string ("name", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_COLOR] = g_param_spec_string ("color", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (props), props);
}

static void
stamp_category_init (StampCategory *self)
{
}

StampCategory *
stamp_category_new (const gchar *id,
                    const gchar *name,
                    const gchar *color)
{
  return g_object_new (STAMP_TYPE_CATEGORY, "id", id, "name", name, "color", color, NULL);
}

const gchar *
stamp_category_get_id (StampCategory *self)
{
  g_return_val_if_fail (STAMP_IS_CATEGORY (self), NULL);
  return self->id;
}

const gchar *
stamp_category_get_name (StampCategory *self)
{
  g_return_val_if_fail (STAMP_IS_CATEGORY (self), NULL);
  return self->name;
}

const gchar *
stamp_category_get_color (StampCategory *self)
{
  g_return_val_if_fail (STAMP_IS_CATEGORY (self), NULL);
  return self->color;
}

/* Microsoft 365 Helper */

GList *
stamp_m365_get_categories_sync (ESource       *source,
                                CamelService  *service,
                                GCancellable  *cancellable,
                                GError       **error)
{
  g_autoptr (CamelSettings) settings = NULL;
  EM365Connection *connection = NULL;
  GSList *categories = NULL;
  GList *result = NULL;
  gboolean success;

  g_return_val_if_fail (E_IS_SOURCE (source), NULL);
  g_return_val_if_fail (CAMEL_IS_SERVICE (service), NULL);

  settings = camel_service_ref_settings (service);
  g_return_val_if_fail (CAMEL_IS_M365_SETTINGS (settings), NULL);

  connection = e_m365_connection_new (source, CAMEL_M365_SETTINGS (settings));
  e_binding_bind_property (service, "proxy-resolver", connection, "proxy-resolver", G_BINDING_SYNC_CREATE);
  success = e_m365_connection_get_categories_sync (connection, NULL, &categories, cancellable, error);

  if (success) {
    for (GSList *iter = categories; iter; iter = iter->next) {
      EM365Category *category = iter->data;
      const gchar *id = e_m365_category_get_id (category);
      const gchar *name = e_m365_category_get_display_name (category);
      const gchar *color = e_m365_category_get_color (category);

      if (id && name)
        result = g_list_prepend (result, stamp_category_new (id, name, color));
    }
  }

  g_slist_free_full (categories, (GDestroyNotify)json_object_unref);

  return g_list_reverse (result);
}

static void
stamp_categories_free (GList *list)
{
  g_list_free_full (list, g_object_unref);
}

static void
categories_thread_func (GTask        *task,
                        gpointer      source_object,
                        gpointer      task_data,
                        GCancellable *cancellable)
{
  CamelService *service = task_data;
  GError *error = NULL;
  GList *result;

  result = stamp_m365_get_categories_sync (E_SOURCE (source_object), service, cancellable, &error);
  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, result, (GDestroyNotify)stamp_categories_free);
}

void
stamp_m365_get_categories_async (ESource             *source,
                                 CamelService        *service,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (E_IS_SOURCE (source));
  g_return_if_fail (CAMEL_IS_SERVICE (service));

  task = g_task_new (source, cancellable, callback, user_data);
  g_task_set_source_tag (task, stamp_m365_get_categories_async);
  g_task_set_task_data (task, g_object_ref (service), g_object_unref);
  g_task_run_in_thread (task, categories_thread_func);
}

GList *
stamp_m365_get_categories_finish (ESource       *source,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  g_return_val_if_fail (E_IS_SOURCE (source), NULL);
  g_return_val_if_fail (g_task_is_valid (result, source), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
