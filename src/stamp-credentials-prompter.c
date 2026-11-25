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

#include "stamp-credentials-prompter.h"

#include <libedataserverui4/libedataserverui4.h>

typedef struct _TryCredentialsData {
  CamelService *service;
  const gchar *mechanism;
} TryCredentialsData;

static gboolean
try_credentials_sync (ECredentialsPrompter    *prompter,
                      ESource                 *source,
                      const ENamedParameters  *credentials,
                      gboolean                *out_authenticated,
                      gpointer                 user_data,
                      GCancellable            *cancellable,
                      GError                 **error)
{
  TryCredentialsData *data = user_data;
  g_autofree char *credential_name = NULL;
  CamelAuthenticationResult result;
  GError *local_error = NULL;

  g_return_val_if_fail (E_IS_SOURCE (source), FALSE);
  g_return_val_if_fail (credentials != NULL, FALSE);
  g_return_val_if_fail (out_authenticated != NULL, FALSE);
  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (CAMEL_IS_SERVICE (data->service), FALSE);

  if (e_source_has_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION)) {
    ESourceAuthentication *auth_extension;

    auth_extension = e_source_get_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION);
    credential_name = e_source_authentication_dup_credential_name (auth_extension);

    if (!credential_name || !*credential_name) {
      g_clear_pointer (&credential_name, g_free);
    }
  }

  camel_service_set_password (data->service, e_named_parameters_get (credentials,
                                                                     credential_name ? credential_name : E_SOURCE_CREDENTIAL_PASSWORD));

  result = camel_service_authenticate_sync (data->service, data->mechanism, cancellable, &local_error);
  if (local_error) {
    g_propagate_error (error, local_error);
    return FALSE;
  }

  *out_authenticated = result == CAMEL_AUTHENTICATION_ACCEPTED;

  if (*out_authenticated) {
    ESourceCredentialsProvider *credentials_provider;
    ESource *cred_source;

    credentials_provider = e_credentials_prompter_get_provider (prompter);
    cred_source = e_source_credentials_provider_ref_credentials_source (credentials_provider, source);

    if (cred_source)
      e_source_invoke_authenticate_sync (cred_source, credentials, cancellable, NULL);

    g_clear_object (&cred_source);
  }

  return result == CAMEL_AUTHENTICATION_REJECTED;
}

gboolean
stamp_credentials_prompt_sync (StampSession  *session,
                               ESource       *source,
                               CamelService  *service,
                               const gchar   *mechanism,
                               GCancellable  *cancellable,
                               GError       **error,
                               gpointer       user_data)
{
  ECredentialsPrompter *prompter;
  TryCredentialsData data;

  data.service = service;
  data.mechanism = mechanism;
  prompter = e_credentials_prompter_new (stamp_session_get_registry (session));

  return e_credentials_prompter_loop_prompt_sync (prompter,
                                                  source,
                                                  E_CREDENTIALS_PROMPTER_PROMPT_FLAG_ALLOW_SOURCE_SAVE,
                                                  try_credentials_sync,
                                                  &data,
                                                  cancellable,
                                                  error);
}
