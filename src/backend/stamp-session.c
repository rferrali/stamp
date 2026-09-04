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
#include <libedataserverui4/libedataserverui4.h>
#include <nss.h>
#include <nssb64.h>
#include <pk11pub.h>
#include <secmod.h>

#include "stamp-account.h"
#include "stamp-helper.h"
#include "stamp-signature.h"

static StampSession *_session = NULL;

struct _StampSession {
  CamelSession parent_instance;

  ESourceRegistry *registry;

  GList *accounts;
  GList *signatures;
  GCancellable *cancellable;
};

typedef struct _TryCredentialsData {
  CamelService *service;
  const gchar *mechanism;
} TryCredentialsData;

G_DEFINE_FINAL_TYPE (StampSession, stamp_session, CAMEL_TYPE_SESSION);

enum {
  ACCOUNT_ADDED,
  ACCOUNT_REMOVED,
  ACCOUNT_CHANGED,
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

static GPtrArray *
stamp_accounts_load_finish (GAsyncResult  *res,
                            GError       **error)
{
  return g_task_propagate_pointer (G_TASK (res), error);
}

static void
on_account_ready (GObject      *src,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  StampAccount *account = STAMP_ACCOUNT (src);
  g_autoptr (GError) error = NULL;

  if (!stamp_account_init_finish (res, &error)) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("%s: Account init failed: %s", G_STRFUNC, error ? error->message : "");

    return;
  }

  g_debug ("%s: '%s' services ready", G_STRFUNC, stamp_account_get_name (account));
}

static void
on_accounts_loaded (GObject      *src,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  StampSession *self = STAMP_SESSION (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) accounts = stamp_accounts_load_finish (res, &error);

  if (error) {
    g_warning ("%s: Error loading accounts: %s\n", G_STRFUNC, error->message);
    goto out;
  }

  g_debug ("%s: %u account(s) found", G_STRFUNC, accounts->len);

  if (accounts->len == 0)
    goto out;

  for (guint idx = 0; idx < accounts->len; idx++) {
    StampAccount *account = g_ptr_array_index (accounts, idx);

    g_debug ("%s: Loading account %s", G_STRFUNC, stamp_account_get_name (account));
    self->accounts = g_list_append (self->accounts, account);
    g_signal_emit (self, signals[ACCOUNT_ADDED], 0, account, NULL);

    stamp_account_init_async (g_object_ref (account), self->cancellable, on_account_ready, self);
  }

out:
  g_signal_emit (self, signals[STARTUP_FINISHED], 0);
}

static GPtrArray *
stamp_session_load_accounts_from_registry (ESourceRegistry *registry)
{
  GPtrArray *accounts = g_ptr_array_new_with_free_func (g_object_unref);
  g_autolist (ESource) collections = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_COLLECTION);

  for (GList *l = collections; l; l = l->next) {
    ESource *col = E_SOURCE (l->data);
    StampAccount *account = stamp_account_new (col, registry);
    g_autolist (ESource) children = NULL;

    g_debug ("%s: %s", G_STRFUNC, e_source_get_display_name (col));

    children = e_source_registry_list_sources (registry, NULL);
    for (GList *child = children; child; child = g_list_next (child)) {
      ESource *src = E_SOURCE (child->data);
      const gchar *parent = e_source_get_parent (src);

      if (g_strcmp0 (parent, stamp_account_get_uid (account)) != 0)
        continue;

      g_debug ("%s: |- %s ", G_STRFUNC, e_source_get_display_name (src));

      if (e_source_has_extension (src, E_SOURCE_EXTENSION_MAIL_ACCOUNT)) {
        stamp_account_add_mail (account, src);
      } else if (e_source_has_extension (src, E_SOURCE_EXTENSION_CALENDAR)) {
        /* stamp_account_add_calendar (account, src); */
      } else if (e_source_has_extension (src, E_SOURCE_EXTENSION_ADDRESS_BOOK)) {
        stamp_account_add_address_book (account, src);
      } else if (e_source_has_extension (src, E_SOURCE_EXTENSION_MAIL_IDENTITY)) {
        stamp_account_add_mail_identity (account, src);
      } else if (e_source_has_extension (src, E_SOURCE_EXTENSION_MAIL_TRANSPORT)) {
        stamp_account_add_mail_transport (account, src);
      } else if (e_source_has_extension (src, E_SOURCE_EXTENSION_TASK_LIST)) {
        g_debug ("  (Task List)");
      }
    }

    g_ptr_array_add (accounts, account);
  }

  return accounts;
}

static StampAccount *
stamp_session_find_account_by_uid (GList       *list,
                                   const gchar *uid)
{
  for (GList *iter = list; iter && iter->data; iter = g_list_next (iter)) {
    StampAccount *account = STAMP_ACCOUNT (iter->data);

    if (g_strcmp0 (stamp_account_get_uid (account), uid) == 0)
      return account;
  }

  return NULL;
}

static void
on_source_changed (ESourceRegistry *registry,
                   ESource         *source,
                   gpointer         user_data)
{
  StampSession *self = STAMP_SESSION (user_data);
  const gchar *parent_uid = e_source_get_parent (source);
  StampAccount *account = stamp_session_find_account_by_uid (self->accounts, parent_uid);

  if (!account) {
    /* No account means that it is a change in the collection */
    account = stamp_session_find_account_by_uid (self->accounts, e_source_get_uid (source));

    if (account) {
      stamp_account_set_name (account, e_source_get_display_name (source));
      g_debug ("%s: Account name updated %s", G_STRFUNC, stamp_account_get_name (account));
      g_signal_emit (self, signals[ACCOUNT_CHANGED], 0, account, NULL);
    }
    return;
  }

  if (e_source_has_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT)) {
    stamp_account_mail_changed (account, source);
  } else if (e_source_has_extension (source, E_SOURCE_EXTENSION_ADDRESS_BOOK)) {
    stamp_account_contacts_changed (account, source);
  }
}

static void
on_signature_loaded (GObject      *source_object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  StampSession *self = STAMP_SESSION (user_data);
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
stamp_session_load_signatures (StampSession *self)
{
  g_autolist (ESource) sources = NULL;

  sources = e_source_registry_list_sources (self->registry, E_SOURCE_EXTENSION_MAIL_SIGNATURE);
  for (GList *iter = sources; iter != NULL; iter = iter->next) {
    ESource *source = E_SOURCE (iter->data);

    e_source_mail_signature_load (source, G_PRIORITY_DEFAULT, self->cancellable, on_signature_loaded, self);
  }
}

static void
on_registry_ready_for_load (GObject      *src,
                            GAsyncResult *res,
                            gpointer      user_data)
{
  g_autoptr (GTask) task = G_TASK (user_data);
  StampSession *self = STAMP_SESSION (g_task_get_source_object (task));
  g_autoptr (GError) error = NULL;
  g_autoptr (ESourceRegistry) registry = e_source_registry_new_finish (res, &error);

  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  self->registry = g_object_ref (registry);
  g_signal_connect_object (self->registry, "source-changed", G_CALLBACK (on_source_changed), self, G_CONNECT_DEFAULT);

  stamp_session_load_signatures (self);

  g_task_return_pointer (task, stamp_session_load_accounts_from_registry (self->registry), (GDestroyNotify)g_ptr_array_unref);
}

static void
stamp_session_init (StampSession *self)
{
  GNetworkMonitor *network_monitor = e_network_monitor_get_default ();
  GTask *task;
  g_autofree char *nssdb = g_build_filename (g_get_home_dir (), ".pki", "nssdb", NULL);

  self->cancellable = g_cancellable_new ();

  camel_init (nssdb, TRUE);
  g_signal_connect_object (G_OBJECT (self), "user-alert", G_CALLBACK (on_user_alert), self, G_CONNECT_DEFAULT);

  PK11_SetPasswordFunc (stamp_session_pk11_password);

  camel_session_set_network_monitor (CAMEL_SESSION (self), network_monitor);
  g_signal_connect_object (network_monitor, "network-changed", G_CALLBACK (on_network_changed), self, G_CONNECT_DEFAULT);
  camel_session_set_online (CAMEL_SESSION (self), TRUE);

  task = g_task_new (self, self->cancellable, on_accounts_loaded, self);
  g_task_set_source_tag (task, stamp_session_init);
  e_source_registry_new (self->cancellable, on_registry_ready_for_load, task);
}

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
    ECredentialsPrompter *prompter;
    TryCredentialsData data;

    data.service = service;
    data.mechanism = mechanism;
    prompter = e_credentials_prompter_new (self->registry);
    authenticated = e_credentials_prompter_loop_prompt_sync (prompter,
                                                             source,
                                                             E_CREDENTIALS_PROMPTER_PROMPT_FLAG_ALLOW_SOURCE_SAVE,
                                                             try_credentials_sync,
                                                             &data,
                                                             cancellable,
                                                             error);
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

  for (GList *iter = self->accounts; iter && iter->data; iter = g_list_next (iter)) {
    StampAccount *account = STAMP_ACCOUNT (iter->data);
    StampMailService *mail_service = stamp_account_get_mail_service (account);

    if (stamp_mail_service_get_service (mail_service) == service) {
      g_signal_emit (self, signals[ACCOUNT_REMOVED], 0, account, NULL);
      self->accounts = g_list_remove (self->accounts, account);
      return;
    }
  }
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
  g_clear_list (&self->accounts, g_object_unref);
  g_clear_list (&self->signatures, stamp_signature_clear);

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

  signals[ACCOUNT_ADDED] = g_signal_new ("account-added", G_OBJECT_CLASS_TYPE (klass),
                                         G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE,
                                         1, STAMP_TYPE_ACCOUNT);

  signals[ACCOUNT_REMOVED] = g_signal_new ("account-removed", G_OBJECT_CLASS_TYPE (klass),
                                           G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                           0, NULL, NULL, NULL,
                                           G_TYPE_NONE,
                                           1, STAMP_TYPE_ACCOUNT);

  signals[ACCOUNT_CHANGED] = g_signal_new ("account-changed", G_OBJECT_CLASS_TYPE (klass),
                                           G_SIGNAL_RUN_FIRST | G_SIGNAL_RUN_LAST,
                                           0, NULL, NULL, NULL,
                                           G_TYPE_NONE,
                                           1, STAMP_TYPE_ACCOUNT);

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

GList *
stamp_session_get_accounts (StampSession *self)
{
  return self->accounts;
}

StampAccount *
stamp_session_find_account_by_mail_uid (StampSession *self,
                                        const gchar  *mail_uid)
{
  if (!mail_uid)
    return NULL;

  for (GList *iter = self->accounts; iter; iter = iter->next) {
    StampAccount *account = STAMP_ACCOUNT (iter->data);
    StampMailService *mail_service = stamp_account_get_mail_service (account);
    CamelService *service;

    if (!mail_service)
      continue;

    service = stamp_mail_service_get_service (mail_service);
    if (service && g_strcmp0 (camel_service_get_uid (service), mail_uid) == 0)
      return account;
  }

  return NULL;
}

ESourceRegistry *
stamp_session_get_registry (StampSession *self)
{
  return self->registry;
}

void
stamp_session_remove_signature (StampSession   *self,
                                StampSignature *signature)
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

StampSignature *
stamp_session_create_signature (StampSession *self,
                                const gchar  *name,
                                const gchar  *content,
                                const gchar  *mime_type)
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

GList *
stamp_session_get_signatures (StampSession *self)
{
  return self->signatures;
}
