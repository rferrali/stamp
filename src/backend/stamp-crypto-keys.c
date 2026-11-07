/*
 * Copyright 2026 Jan-Michael Brummer
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

#include "stamp-crypto-keys.h"

#include <locale.h>

#include <cert.h>
#include <glib/gi18n.h>
#include <gpgme.h>
#include <pk11pub.h>

static StampCryptoKeys *cached_keys;
static gint64 cached_at;
static GPtrArray *pending_loads;

#define CACHE_MAX_AGE (5 * 60 * G_USEC_PER_SEC)

typedef struct gpgme_context StampGpgmeContext;
typedef struct _gpgme_key StampGpgmeKey;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (StampGpgmeContext, gpgme_release)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (StampGpgmeKey, gpgme_key_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CERTCertList, CERT_DestroyCertList)

StampCryptoKeys *
stamp_crypto_keys_ref (StampCryptoKeys *keys)
{
  return g_atomic_rc_box_acquire (keys);
}

static void
stamp_crypto_keys_clear (StampCryptoKeys *keys)
{
  g_clear_pointer (&keys->pgp_labels, g_ptr_array_unref);
  g_clear_pointer (&keys->pgp_key_ids, g_ptr_array_unref);
  g_clear_pointer (&keys->smime_labels, g_ptr_array_unref);
  g_clear_pointer (&keys->smime_cert_nicknames, g_ptr_array_unref);
}

void
stamp_crypto_keys_unref (StampCryptoKeys *keys)
{
  g_atomic_rc_box_release_full (keys, (GDestroyNotify)stamp_crypto_keys_clear);
}

static void
enumerate_smime_certs (StampCryptoKeys *keys)
{
  g_autoptr (CERTCertList) cert_list = PK11_ListCerts (PK11CertListAll, NULL);

  g_ptr_array_add (keys->smime_labels, g_strdup (_("None")));
  g_ptr_array_add (keys->smime_cert_nicknames, NULL);

  if (!cert_list)
    return;

  for (CERTCertListNode *node = CERT_LIST_HEAD (cert_list); !CERT_LIST_END (node, cert_list); node = CERT_LIST_NEXT (node)) {
    const gchar *nickname = node->cert->nickname;

    if (!nickname || !*nickname)
      continue;

    g_ptr_array_add (keys->smime_labels, g_strdup (nickname));
    g_ptr_array_add (keys->smime_cert_nicknames, g_strdup (nickname));
  }
}

static gboolean
init_gpgme (void)
{
  static gsize initialized = 0;
  static gboolean available = FALSE;

  if (g_once_init_enter (&initialized)) {
    gpgme_error_t err;

    gpgme_check_version (NULL);
    gpgme_set_locale (NULL, LC_CTYPE, setlocale (LC_CTYPE, NULL));
#ifdef LC_MESSAGES
    gpgme_set_locale (NULL, LC_MESSAGES, setlocale (LC_MESSAGES, NULL));
#endif

    err = gpgme_engine_check_version (GPGME_PROTOCOL_OpenPGP);
    if (err != GPG_ERR_NO_ERROR)
      g_warning ("OpenPGP engine not available: %s", gpgme_strerror (err));
    else
      available = TRUE;

    g_once_init_leave (&initialized, 1);
  }

  return available;
}

static void
enumerate_pgp_keys (StampCryptoKeys *keys)
{
  g_autoptr (StampGpgmeContext) ctx = NULL;
  gpgme_error_t err;

  g_ptr_array_add (keys->pgp_labels, g_strdup (_("None")));
  g_ptr_array_add (keys->pgp_key_ids, NULL);

  if (!init_gpgme ())
    return;

  err = gpgme_new (&ctx);
  if (err != GPG_ERR_NO_ERROR) {
    g_warning ("Failed to create gpgme context: %s", gpgme_strerror (err));
    return;
  }

  gpgme_set_protocol (ctx, GPGME_PROTOCOL_OpenPGP);
  gpgme_set_keylist_mode (ctx, GPGME_KEYLIST_MODE_LOCAL);

  err = gpgme_op_keylist_start (ctx, NULL, 0);
  if (err != GPG_ERR_NO_ERROR) {
    g_warning ("Failed to list PGP keys: %s", gpgme_strerror (err));
    return;
  }

  for (;;) {
    g_autoptr (StampGpgmeKey) key = NULL;
    const gchar *key_id;
    const gchar *uid;

    if (gpgme_op_keylist_next (ctx, &key) != GPG_ERR_NO_ERROR)
      break;

    key_id = key->subkeys ? key->subkeys->keyid : NULL;
    uid = key->uids ? key->uids->uid : NULL;

    if (key_id && (key->secret || key->has_sign || key->has_encrypt)) {
      g_ptr_array_add (keys->pgp_labels, g_strdup (uid ? uid : key_id));
      g_ptr_array_add (keys->pgp_key_ids, g_strdup (key_id));
    }
  }

  gpgme_op_keylist_end (ctx);
}

static void
load_keys_thread (GTask        *task,
                  gpointer      source_object,
                  gpointer      task_data,
                  GCancellable *cancellable)
{
  StampCryptoKeys *keys = g_atomic_rc_box_new0 (StampCryptoKeys);

  keys->pgp_labels = g_ptr_array_new_with_free_func (g_free);
  keys->pgp_key_ids = g_ptr_array_new_with_free_func (g_free);
  keys->smime_labels = g_ptr_array_new_with_free_func (g_free);
  keys->smime_cert_nicknames = g_ptr_array_new_with_free_func (g_free);

  enumerate_pgp_keys (keys);
  enumerate_smime_certs (keys);

  g_task_return_pointer (task, keys, (GDestroyNotify)stamp_crypto_keys_unref);
}

static void
on_keys_loaded (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr (GPtrArray) requests = g_steal_pointer (&pending_loads);

  g_clear_pointer (&cached_keys, stamp_crypto_keys_unref);
  cached_keys = g_task_propagate_pointer (G_TASK (result), NULL);
  cached_at = g_get_monotonic_time ();

  for (guint i = 0; i < requests->len; i++) {
    GTask *request = g_ptr_array_index (requests, i);

    g_task_return_pointer (request, stamp_crypto_keys_ref (cached_keys), (GDestroyNotify)stamp_crypto_keys_unref);
  }
}

void
stamp_crypto_keys_load_async (GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  g_autoptr (GTask) request = g_task_new (NULL, cancellable, callback, user_data);
  g_autoptr (GTask) worker = NULL;

  g_task_set_source_tag (request, stamp_crypto_keys_load_async);
  if (g_task_return_error_if_cancelled (request))
    return;

  if (cached_keys && g_get_monotonic_time () - cached_at < CACHE_MAX_AGE) {
    g_task_return_pointer (request, stamp_crypto_keys_ref (cached_keys), (GDestroyNotify)stamp_crypto_keys_unref);
    return;
  }

  if (pending_loads) {
    g_ptr_array_add (pending_loads, g_steal_pointer (&request));
    return;
  }

  pending_loads = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (pending_loads, g_steal_pointer (&request));

  worker = g_task_new (NULL, NULL, on_keys_loaded, NULL);
  g_task_set_source_tag (worker, stamp_crypto_keys_load_async);
  g_task_run_in_thread (worker, load_keys_thread);
}

StampCryptoKeys *
stamp_crypto_keys_load_finish (GAsyncResult  *result,
                               GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result, stamp_crypto_keys_load_async), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
