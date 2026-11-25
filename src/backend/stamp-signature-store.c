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

#include "stamp-signature-store.h"

#include <string.h>

struct _StampSignatureStore {
  GObject parent_instance;

  ESourceRegistry *registry;
  GList *signatures;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (StampSignatureStore, stamp_signature_store, G_TYPE_OBJECT);

static void
on_signature_loaded (GObject      *source_object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  StampSignatureStore *self = STAMP_SIGNATURE_STORE (user_data);
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;
  ESourceMailSignature *ext;
  ESource *source = E_SOURCE (source_object);
  StampSignature *signature;

  if (!e_source_mail_signature_load_finish (source, result, &contents, &length, &error)) {
    g_debug ("Error loading signature: %s", error->message);
    return;
  }

  ext = e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_SIGNATURE);

  signature = stamp_signature_new (source, e_source_mail_signature_get_mime_type (ext), contents);
  self->signatures = g_list_append (self->signatures, signature);
}

static void
stamp_signature_store_dispose (GObject *object)
{
  StampSignatureStore *self = STAMP_SIGNATURE_STORE (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->registry);
  g_clear_list (&self->signatures, stamp_signature_clear);

  G_OBJECT_CLASS (stamp_signature_store_parent_class)->dispose (object);
}

static void
stamp_signature_store_class_init (StampSignatureStoreClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = stamp_signature_store_dispose;
}

static void
stamp_signature_store_init (StampSignatureStore *self)
{
  self->cancellable = g_cancellable_new ();
}

StampSignatureStore *
stamp_signature_store_new (void)
{
  return g_object_new (STAMP_TYPE_SIGNATURE_STORE, NULL);
}

void
stamp_signature_store_load (StampSignatureStore *self,
                            ESourceRegistry     *registry)
{
  g_autolist (ESource) sources = NULL;

  g_return_if_fail (STAMP_IS_SIGNATURE_STORE (self));

  g_set_object (&self->registry, registry);

  sources = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_MAIL_SIGNATURE);
  for (GList *iter = sources; iter != NULL; iter = iter->next) {
    ESource *source = E_SOURCE (iter->data);

    e_source_mail_signature_load (source, G_PRIORITY_DEFAULT, self->cancellable, on_signature_loaded, self);
  }
}

GList *
stamp_signature_store_get_signatures (StampSignatureStore *self)
{
  return self->signatures;
}

StampSignature *
stamp_signature_store_create (StampSignatureStore *self,
                              const gchar         *name,
                              const gchar         *content,
                              const gchar         *mime_type)
{
  g_autoptr (ESource) new_source = NULL;
  g_autoptr (ESource) source = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *uid = NULL;
  ESourceMailSignature *ext;
  StampSignature *signature;

  new_source = e_source_new (NULL, NULL, &error);
  if (error) {
    g_warning ("%s: Could not create signature source: %s", G_STRFUNC, error->message);
    return NULL;
  }

  e_source_set_display_name (new_source, name);

  ext = e_source_get_extension (new_source, E_SOURCE_EXTENSION_MAIL_SIGNATURE);
  e_source_mail_signature_replace (new_source, content, strlen (content), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
  e_source_mail_signature_set_mime_type (ext, mime_type);

  e_source_registry_commit_source_sync (self->registry, new_source, NULL, &error);
  if (error) {
    g_warning ("%s: Could not commit signature source: %s", G_STRFUNC, error->message);
    return NULL;
  }

  uid = g_strdup (e_source_get_uid (new_source));

  source = e_source_registry_ref_source (self->registry, uid);
  if (!source) {
    g_warning ("%s: Could not ref committed signature", G_STRFUNC);
    return NULL;
  }

  signature = stamp_signature_new (source, mime_type, content);
  self->signatures = g_list_append (self->signatures, signature);

  return signature;
}

void
stamp_signature_store_remove (StampSignatureStore *self,
                              StampSignature      *signature)
{
  g_autoptr (GError) error = NULL;

  e_source_remove_sync (stamp_signature_get_source (signature), self->cancellable, &error);
  if (error) {
    g_warning ("%s: Could not remove signature: %s", G_STRFUNC, error->message);
    return;
  }

  self->signatures = g_list_remove (self->signatures, signature);
  stamp_signature_clear (signature);
}

gboolean
stamp_signature_store_update (StampSignatureStore *self,
                              StampSignature      *signature,
                              const gchar         *name,
                              const gchar         *html)
{
  g_autoptr (GError) error = NULL;

  stamp_signature_set_name (signature, name);
  e_source_set_display_name (stamp_signature_get_source (signature), name);
  stamp_signature_save (signature, html);

  e_source_registry_commit_source_sync (self->registry, stamp_signature_get_source (signature), NULL, &error);
  if (error) {
    g_warning ("%s: Could not commit signature: %s", G_STRFUNC, error->message);
    return FALSE;
  }

  return TRUE;
}
