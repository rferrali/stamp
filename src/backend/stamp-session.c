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

#include "stamp-session.h"

#include <libebook/libebook.h>
#include <libedata-book/libedata-book.h>
#include <nss.h>
#include <nssb64.h>
#include <pk11pub.h>
#include <secmod.h>

#include "stamp-account-manager.h"
#include "stamp-account.h"
#include "stamp-helper.h"
#include "stamp-m365-calendar.h"
#include "stamp-signature-store.h"

static StampSession *_session = NULL;

struct _StampSession {
  CamelSession parent_instance;

  ESourceRegistry *registry;

  StampAccountManager *account_manager;
  StampSignatureStore *signature_store;
  GCancellable *cancellable;

  StampSessionCredentialsFunc credentials_func;
  gpointer credentials_data;
};

G_DEFINE_FINAL_TYPE (StampSession, stamp_session, CAMEL_TYPE_SESSION);

enum {
  PK11_PASSWORD,
  STARTUP_FINISHED,
  LAST_SIGNAL,
};

static gint signals[LAST_SIGNAL] = { 0 };

static void
on_user_alert (CamelSession *session,
               CamelService *service,
               gint          type,
               gchar        *message)
{
  g_warning ("%s: %s", G_STRFUNC, message);
}

static void
on_network_changed (GNetworkMonitor *monitor,
                    gboolean         state,
                    gpointer         user_data)
{
  StampSession *self = STAMP_SESSION (user_data);

  camel_session_set_online (CAMEL_SESSION (self), state);
}

static gchar *
stamp_session_pk11_password (PK11SlotInfo *slot,
                             PRBool        retry,
                             gpointer      arg)
{
  g_autofree char *pwd = NULL;
  gchar *nsspwd;

  /* For tokens with CKF_PROTECTED_AUTHENTICATION_PATH we
   * need to return a non-empty but unused password */
  if (PK11_ProtectedAuthenticationPath (slot))
    return PORT_Strdup ("");

  g_signal_emit (G_OBJECT (stamp_session_get_default ()), signals[PK11_PASSWORD], 0, slot, retry, &pwd);
  if (!pwd)
    return NULL;

  nsspwd = PORT_Strdup (pwd);
  memset (pwd, 0, strlen (pwd));

  return nsspwd;
}

static void
on_accounts_loaded (GObject      *src,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  StampSession *self = STAMP_SESSION (user_data);
  g_autoptr (GError) error = NULL;

  if (!stamp_account_manager_load_finish (STAMP_ACCOUNT_MANAGER (src), res, &error))
    g_warning ("%s: Error loading accounts: %s\n", G_STRFUNC, error->message);

  g_signal_emit (self, signals[STARTUP_FINISHED], 0);
}

static void
on_registry_ready_for_load (GObject      *src,
                            GAsyncResult *res,
                            gpointer      user_data)
{
  StampSession *self = STAMP_SESSION (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (ESourceRegistry) registry = e_source_registry_new_finish (res, &error);

  if (error) {
    g_warning ("%s: Error loading accounts: %s\n", G_STRFUNC, error->message);
    g_signal_emit (self, signals[STARTUP_FINISHED], 0);
    return;
  }

  self->registry = g_object_ref (registry);

  stamp_signature_store_load (self->signature_store, self->registry);
  stamp_m365_calendar_sync_colors_async (self->registry, self->cancellable);

  stamp_account_manager_load_async (self->account_manager, self->registry, self->cancellable, on_accounts_loaded, self);
}

static void
on_camel_ready (GObject      *src,
                GAsyncResult *res,
                gpointer      user_data)
{
  StampSession *self = STAMP_SESSION (src);
  GNetworkMonitor *network_monitor = e_network_monitor_get_default ();

  PK11_SetPasswordFunc (stamp_session_pk11_password);

  camel_session_set_network_monitor (CAMEL_SESSION (self), network_monitor);
  g_signal_connect_object (network_monitor, "network-changed", G_CALLBACK (on_network_changed), self, G_CONNECT_DEFAULT);
  camel_session_set_online (CAMEL_SESSION (self), TRUE);

  e_source_registry_new (self->cancellable, on_registry_ready_for_load, self);
}

static void
camel_init_thread (GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data,
                   GCancellable *cancellable)
{
  camel_init (task_data, TRUE);
  g_application_unmark_busy (g_application_get_default ());

  g_task_return_boolean (task, TRUE);
}

static void
stamp_session_init (StampSession *self)
{
  g_autoptr (GTask) task = NULL;
  g_autofree char *nssdb = g_build_filename (g_get_home_dir (), ".pki", "nssdb", NULL);

  g_application_mark_busy (g_application_get_default ());
  self->cancellable = g_cancellable_new ();
  self->account_manager = stamp_account_manager_new ();
  self->signature_store = stamp_signature_store_new ();

  g_signal_connect_object (G_OBJECT (self), "user-alert", G_CALLBACK (on_user_alert), self, G_CONNECT_DEFAULT);

  task = g_task_new (self, NULL, on_camel_ready, NULL);
  g_task_set_source_tag (task, stamp_session_init);
  g_task_set_task_data (task, g_steal_pointer (&nssdb), g_free);
  g_task_run_in_thread (task, camel_init_thread);
}

static gboolean
authenticate_sync (CamelSession  *session,
                   CamelService  *service,
                   const gchar   *mechanism,
                   GCancellable  *cancellable,
                   GError       **error)
{
  StampSession *self = STAMP_SESSION (session);
  CamelServiceAuthType *authtype = NULL;
  gboolean try_empty_password = FALSE;
  CamelAuthenticationResult result = CAMEL_AUTHENTICATION_REJECTED;
  GError *local_error = NULL;
  g_autoptr (ESource) source = NULL;
  const gchar *uid;
  gboolean authenticated;

  /* Treat a mechanism name of "none" as NULL. */
  if (g_strcmp0 (mechanism, "none") == 0)
    mechanism = NULL;

  /* APOP is one case where a non-SASL mechanism name is passed, so
   * don't bail if the CamelServiceAuthType struct comes back NULL. */
  if (mechanism != NULL)
    authtype = camel_sasl_authtype (mechanism);

  /* If the SASL mechanism does not involve a user
   * password, then it gets one shot to authenticate. */
  if (authtype != NULL && !authtype->need_password) {
    result = camel_service_authenticate_sync (service, mechanism, cancellable, &local_error);
    if (local_error) {
      g_propagate_error (error, local_error);
      return FALSE;
    }
    return result == CAMEL_AUTHENTICATION_ACCEPTED;
  }

  /* Some SASL mechanisms can attempt to authenticate without a
   * user password being provided (e.g. single-sign-on credentials),
   * but can fall back to a user password.  Handle that case next. */
  if (mechanism != NULL) {
    CamelProvider *provider;
    g_autoptr (CamelSasl) sasl = NULL;
    const gchar *service_name;

    provider = camel_service_get_provider (service);
    service_name = provider->protocol;

    /* XXX Would be nice if camel_sasl_try_empty_password_sync()
     *     returned the result in an "out" parameter so it's
     *     easier to distinguish errors from a "no" answer.
     * YYY There are precisely two states. Either we appear to
     *     have credentials (although we don't yet know if the
     *     server would *accept* them, of course). Or we don't
     *     have any credentials, and we can't even try. There
     *     is no middle ground.
     *     N.B. For 'have credentials', read 'the ntlm_auth
     *          helper exists and at first glance seems to
     *          be responding sanely'. */
    sasl = camel_sasl_new (service_name, mechanism, service);
    if (sasl != NULL) {
      try_empty_password =
        camel_sasl_try_empty_password_sync (
          sasl, cancellable, &local_error);
    }
  }

  /* Abort authentication if we got cancelled.
   * Otherwise clear any errors and press on. */
  if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return FALSE;

  g_clear_error (&local_error);

  uid = camel_service_get_uid (service);
  source = e_source_registry_ref_source (self->registry, uid);

  if (!source) {
    g_set_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE, ("No data source found for UID '%s'"), uid);
    return FALSE;
  }

  result = CAMEL_AUTHENTICATION_REJECTED;

  if (try_empty_password) {
    result = camel_service_authenticate_sync (service, mechanism, cancellable, &local_error);
    if (local_error) {
      g_propagate_error (error, local_error);
      return FALSE;
    }
  }

  if (result == CAMEL_AUTHENTICATION_REJECTED) {
    if (!self->credentials_func) {
      g_set_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE, "No credentials prompt available");
      return FALSE;
    }

    authenticated = self->credentials_func (self, source, service, mechanism, cancellable, error, self->credentials_data);
  } else {
    authenticated = (result == CAMEL_AUTHENTICATION_ACCEPTED);
  }

  return authenticated;
}

static CamelService *
add_service (CamelSession       *session,
             const gchar        *uid,
             const gchar        *protocol,
             CamelProviderType   type,
             GError            **error)
{
  CamelService *service;
  StampSession *self = STAMP_SESSION (session);
  g_autoptr (GError) local_error = NULL;

  service = CAMEL_SESSION_CLASS (stamp_session_parent_class)->add_service (
    session,
    uid,
    protocol,
    type,
    error);

  if (CAMEL_IS_SERVICE (service)) {
    g_autoptr (ESource) source = e_source_registry_ref_source (self->registry, uid);
    const gchar *extension_name = e_source_camel_get_extension_name (protocol);
    g_autoptr (ESource) extension_source = e_source_registry_find_extension (self->registry, source, extension_name);

    if (extension_source) {
      g_clear_object (&source);
      source = g_steal_pointer (&extension_source);
    }

    e_source_camel_configure_service (source, service);
  }

  return service;
}

static void
remove_service (CamelSession *session,
                CamelService *service)
{
  StampSession *self = STAMP_SESSION (session);

  CAMEL_SESSION_CLASS (stamp_session_parent_class)->remove_service (session, service);

  /* TODO: Handle more than offline store */
  if (!CAMEL_IS_OFFLINE_STORE (service))
    return;

  stamp_account_manager_remove_by_service (self->account_manager, service);
}

static CamelFilterDriver *
get_filter_driver (CamelSession  *session,
                   const gchar   *type,
                   CamelFolder   *folder,
                   GError       **error)
{
  CamelFilterDriver *filter_driver = camel_filter_driver_new (session);

  return filter_driver;
}

static gboolean
get_oauth2_access_token_sync (CamelSession  *session,
                              CamelService  *service,
                              gchar        **out_access_token,
                              gint          *out_expires_in,
                              GCancellable  *cancellable,
                              GError       **error)
{
  StampSession *self;
  g_autoptr (ESource) source = NULL;
  g_autoptr (ESource) cred_source = NULL;
  GError *local_error = NULL;
  gboolean success;

  g_return_val_if_fail (STAMP_IS_SESSION (session), FALSE);
  g_return_val_if_fail (CAMEL_IS_SERVICE (service), FALSE);

  self = STAMP_SESSION (session);
  source = e_source_registry_ref_source (self->registry, camel_service_get_uid (service));
  if (!source) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 ("Corresponding source for service with UID “%s” not found"),
                 camel_service_get_uid (service));

    return FALSE;
  }

  cred_source = e_source_registry_find_extension (self->registry, source, E_SOURCE_EXTENSION_COLLECTION);
  if (!cred_source || !e_util_can_use_collection_as_credential_source (cred_source, source)) {
    g_set_object (&cred_source, source);
  }

  success = e_source_get_oauth2_access_token_sync (cred_source, cancellable, out_access_token, out_expires_in, &local_error);

  /* The Connection Refused error can be returned when the OAuth2 token is expired or
     when its refresh failed for some reason. In that case change the error domain/code,
     thus the other Camel/mail code understands it. */
  if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED) ||
      g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
    local_error->domain = CAMEL_SERVICE_ERROR;
    local_error->code = CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE;

    e_source_invoke_credentials_required_sync (cred_source, E_SOURCE_CREDENTIALS_REASON_REJECTED, "", 0, local_error, cancellable, NULL);
  }

  if (local_error)
    g_propagate_error (error, local_error);

  return success;
}

static void
stamp_session_dispose (GObject *object)
{
  StampSession *self = STAMP_SESSION (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_object (&self->registry);
  g_clear_object (&self->account_manager);
  g_clear_object (&self->signature_store);

  camel_shutdown ();

  G_OBJECT_CLASS (stamp_session_parent_class)->dispose (object);
}

static void
stamp_session_class_init (StampSessionClass *klass)
{
  CamelSessionClass *session_class;
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  session_class = CAMEL_SESSION_CLASS (klass);
  session_class->authenticate_sync = authenticate_sync;
  session_class->add_service = add_service;
  session_class->remove_service = remove_service;
  session_class->get_filter_driver = get_filter_driver;
  session_class->get_oauth2_access_token_sync = get_oauth2_access_token_sync;

  object_class->dispose = stamp_session_dispose;

  signals[PK11_PASSWORD] = g_signal_new ("pk11-password", G_OBJECT_CLASS_TYPE (klass),
                                         G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE,
                                         3, G_TYPE_POINTER, G_TYPE_INT, G_TYPE_POINTER);

  signals[STARTUP_FINISHED] = g_signal_new ("startup-finished", G_OBJECT_CLASS_TYPE (klass),
                                            G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE,
                                            0);
}

StampSession *
stamp_session_get_default (void)
{
  if (!_session) {
    _session = g_object_new (STAMP_TYPE_SESSION,
                             "user-data-dir", stamp_get_data_dir (),
                             "user-cache-dir", stamp_get_cache_dir (),
                             NULL);
  }

  return _session;
}

void
stamp_session_set_credentials_func (StampSession                *self,
                                    StampSessionCredentialsFunc  func,
                                    gpointer                     user_data)
{
  g_return_if_fail (STAMP_IS_SESSION (self));

  self->credentials_func = func;
  self->credentials_data = user_data;
}

StampAccountManager *
stamp_session_get_account_manager (StampSession *self)
{
  return self->account_manager;
}

StampSignatureStore *
stamp_session_get_signature_store (StampSession *self)
{
  return self->signature_store;
}

ESourceRegistry *
stamp_session_get_registry (StampSession *self)
{
  return self->registry;
}
