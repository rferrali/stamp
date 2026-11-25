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

#include "stamp-signature.h"

struct _StampSignature {
  gchar *name;
  gchar *mime_type;
  gchar *content;
  ESource *source;
  GCancellable *cancellable;
};

static void
on_signature_replace (GObject      *source_object,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  ESource *source = E_SOURCE (source_object);

  if (!e_source_mail_signature_replace_finish (source, res, &error)) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("%s: Could not save signature: %s", G_STRFUNC, error->message);

    return;
  }
}

void
stamp_signature_save (StampSignature *self,
                      const gchar    *html_signature)
{
  ESourceMailSignature *ext;

  g_assert (self);
  g_assert (html_signature);

  ext = e_source_get_extension (self->source, E_SOURCE_EXTENSION_MAIL_SIGNATURE);
  if (!ext)
    return;

  g_set_str (&self->content, html_signature);
  self->mime_type = g_strdup ("text/html");

  e_source_mail_signature_set_mime_type (ext, "text/html");
  e_source_mail_signature_replace (self->source, html_signature, strlen (html_signature), G_PRIORITY_DEFAULT, self->cancellable, on_signature_replace, NULL);
}

void
stamp_signature_set_name (StampSignature *self,
                          const gchar    *name)
{
  g_assert (self);

  g_set_str (&self->name, name);
}

void
stamp_signature_clear (gpointer user_data)
{
  StampSignature *self = user_data;

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_object (&self->source);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->mime_type, g_free);
  g_clear_pointer (&self->content, g_free);

  g_clear_pointer (&self, g_free);
}

StampSignature *
stamp_signature_new (ESource     *source,
                     const gchar *mime_type,
                     const gchar *content)
{
  StampSignature *self = g_new0 (StampSignature, 1);

  self->name = g_strdup (e_source_get_display_name (source));
  self->mime_type = g_strdup (mime_type);
  self->content = g_strdup (content);
  self->source = g_object_ref (source);
  self->cancellable = g_cancellable_new ();

  return self;
}

const gchar *
stamp_signature_get_mime_type (StampSignature *self)
{
  g_assert (self);
  return self->mime_type;
}

const gchar *
stamp_signature_get_content (StampSignature *self)
{
  g_assert (self);
  return self->content;
}

const gchar *
stamp_signature_get_name (StampSignature *self)
{
  g_assert (self);
  return self->name;
}

ESource *
stamp_signature_get_source (StampSignature *self)
{
  g_assert (self);
  return self->source;
}
