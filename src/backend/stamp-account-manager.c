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

#include "stamp-account-manager.h"

struct _StampAccountManager {
  GObject parent_instance;

  GList *accounts;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (StampAccountManager, stamp_account_manager, G_TYPE_OBJECT);

enum {
  ACCOUNT_ADDED,
  ACCOUNT_REMOVED,
  ACCOUNT_CHANGED,
  LAST_SIGNAL,
};

static gint signals[LAST_SIGNAL] = { 0 };

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

static GPtrArray *
load_accounts_from_registry (ESourceRegistry *registry)
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
find_account_by_uid (GList       *list,
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
  StampAccountManager *self = STAMP_ACCOUNT_MANAGER (user_data);
  const gchar *parent_uid = e_source_get_parent (source);
  StampAccount *account = find_account_by_uid (self->accounts, parent_uid);

  if (!account) {
    /* No account means that it is a change in the collection */
    account = find_account_by_uid (self->accounts, e_source_get_uid (source));

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
stamp_account_manager_dispose (GObject *object)
{
  StampAccountManager *self = STAMP_ACCOUNT_MANAGER (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_list (&self->accounts, g_object_unref);

  G_OBJECT_CLASS (stamp_account_manager_parent_class)->dispose (object);
}

static void
stamp_account_manager_class_init (StampAccountManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = stamp_account_manager_dispose;

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
}

static void
stamp_account_manager_init (StampAccountManager *self)
{
  self->cancellable = g_cancellable_new ();
}

StampAccountManager *
stamp_account_manager_new (void)
{
  return g_object_new (STAMP_TYPE_ACCOUNT_MANAGER, NULL);
}

void
stamp_account_manager_load_async (StampAccountManager *self,
                                  ESourceRegistry     *registry,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  g_autoptr (GPtrArray) accounts = NULL;

  g_return_if_fail (STAMP_IS_ACCOUNT_MANAGER (self));

  g_task_set_source_tag (task, stamp_account_manager_load_async);

  g_signal_connect_object (registry, "source-changed", G_CALLBACK (on_source_changed), self, G_CONNECT_DEFAULT);

  accounts = load_accounts_from_registry (registry);
  g_debug ("%s: %u account(s) found", G_STRFUNC, accounts->len);

  for (guint idx = 0; idx < accounts->len; idx++) {
    StampAccount *account = g_ptr_array_index (accounts, idx);

    g_debug ("%s: Loading account %s", G_STRFUNC, stamp_account_get_name (account));
    self->accounts = g_list_append (self->accounts, g_object_ref (account));
    g_signal_emit (self, signals[ACCOUNT_ADDED], 0, account, NULL);

    stamp_account_init_async (g_object_ref (account), self->cancellable, on_account_ready, self);
  }

  for (GList *l = self->accounts; l; l = l->next)
    stamp_account_update_color (STAMP_ACCOUNT (l->data));

  g_task_return_boolean (task, TRUE);
}

gboolean
stamp_account_manager_load_finish (StampAccountManager  *self,
                                   GAsyncResult         *result,
                                   GError              **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

GList *
stamp_account_manager_get_accounts (StampAccountManager *self)
{
  return self->accounts;
}

StampAccount *
stamp_account_manager_find_by_mail_uid (StampAccountManager *self,
                                        const gchar         *mail_uid)
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

gboolean
stamp_account_manager_remove_by_service (StampAccountManager *self,
                                         CamelService        *service)
{
  for (GList *iter = self->accounts; iter && iter->data; iter = g_list_next (iter)) {
    StampAccount *account = STAMP_ACCOUNT (iter->data);
    StampMailService *mail_service = stamp_account_get_mail_service (account);

    if (stamp_mail_service_get_service (mail_service) == service) {
      g_signal_emit (self, signals[ACCOUNT_REMOVED], 0, account, NULL);
      self->accounts = g_list_remove (self->accounts, account);
      return TRUE;
    }
  }

  return FALSE;
}
