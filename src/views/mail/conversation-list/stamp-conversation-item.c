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

#include "stamp-conversation-item.h"

#include <glib/gi18n.h>

struct _StampConversationItem {
  GObject parent_instance;

  CamelFolderThread *thread;
  CamelFolderThreadNode *thread_node;
  guint timestamp;
  gchar *senders;
  gchar *subject;
  gchar *sender;
  gchar *service_uid;
  gchar *uid;
  gint64 uid_number;
  gchar *preview;
  gboolean unread;
  gboolean flagged;
  gboolean hidden;
  gboolean important;
};

G_DEFINE_FINAL_TYPE (StampConversationItem, stamp_conversation_item, G_TYPE_OBJECT);

typedef enum {
  PROP_THREAD = 1,
  PROP_THREAD_NODE,
  PROP_UNREAD,
  PROP_FLAGGED,
  PROP_SERVICE_UID,
  PROP_SUBJECT,
  PROP_FROM,
  PROP_PREVIEW,
  PROP_HAS_ATTACHMENT,
  PROP_HAS_CALENDAR,
  PROP_IMPORTANT,
  PROP_DATE,
  PROP_NUM_MESSAGES,
  PROP_LABELS,
  PROP_ANSWERED,
  PROP_FORWARDED,
} StampConversationItemProps;

static GParamSpec *properties[PROP_FORWARDED + 1];

static gboolean
has_thread_flag_one (CamelFolderThreadNode *node,
                     CamelMessageFlags      flag)
{
  gboolean has_flag = FALSE;

  if (!node)
    return FALSE;

  has_flag = (camel_message_info_get_flags (camel_folder_thread_node_get_item (node)) & flag) != 0;
  if (!has_flag) {
    for (CamelFolderThreadNode *child = camel_folder_thread_node_get_child (node); child; child = camel_folder_thread_node_get_next (child)) {
      has_flag |= has_thread_flag_one (child, flag);
      if (has_flag)
        break;
    }
  }

  return has_flag;
}

static gboolean
has_thread_flag_all (CamelFolderThreadNode *node,
                     CamelMessageFlags      flag)
{
  gboolean has_flag = FALSE;

  if (!node)
    return FALSE;

  has_flag = camel_message_info_get_flags (camel_folder_thread_node_get_item (node)) & flag;
  if (!has_flag)
    return FALSE;

  for (CamelFolderThreadNode *child = camel_folder_thread_node_get_child (node); child; child = camel_folder_thread_node_get_next (child)) {
    has_flag = has_thread_flag_all (child, flag);
    if (!has_flag)
      return FALSE;
  }

  return TRUE;
}

const gchar *
stamp_conversation_item_get_subject (StampConversationItem *self)
{
  if (!self->subject) {
    const CamelMessageInfo *info = NULL;

    for (CamelFolderThreadNode *child = camel_folder_thread_node_get_child (self->thread_node); child; child = camel_folder_thread_node_get_next (child)) {
      info = camel_folder_thread_node_get_item (child);
    }

    if (!info)
      info = camel_folder_thread_node_get_item (self->thread_node);

    if (!info)
      return _("Unknown");

    self->subject = g_strdup (camel_message_info_get_subject (info));
  }

  return self->subject;
}

void
stamp_conversation_item_notify_unread (StampConversationItem *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_UNREAD]);
}

static void
stamp_conversation_item_get_property (GObject    *object,
                                      guint       property_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  StampConversationItem *self = STAMP_CONVERSATION_ITEM (object);

  switch ((StampConversationItemProps)property_id) {
    case PROP_THREAD:
      g_value_set_object (value, self->thread);
      break;
    case PROP_THREAD_NODE:
      g_value_set_pointer (value, stamp_conversation_item_get_node (self));
      break;
    case PROP_SERVICE_UID:
      g_value_set_string (value, stamp_conversation_item_get_uid (self));
      break;
    case PROP_UNREAD:
      g_value_set_boolean (value, stamp_conversation_item_get_unread (self));
      break;
    case PROP_FLAGGED:
      g_value_set_boolean (value, stamp_conversation_item_get_flagged (self));
      break;
    case PROP_SUBJECT:
      g_value_set_string (value, stamp_conversation_item_get_subject (self));
      break;
    case PROP_FROM:
      g_value_set_string (value, stamp_conversation_item_get_from (self));
      break;
    case PROP_PREVIEW:
      g_value_set_string (value, stamp_conversation_item_get_preview (self));
      break;
    case PROP_HAS_ATTACHMENT:
      g_value_set_boolean (value, stamp_conversation_item_has_attachment (self));
      break;
    case PROP_HAS_CALENDAR:
      g_value_set_boolean (value, stamp_conversation_item_get_calendar (self));
      break;
    case PROP_IMPORTANT:
      g_value_set_boolean (value, stamp_conversation_item_is_important (self));
      break;
    case PROP_DATE:
      g_value_set_ulong (value, stamp_conversation_item_get_date (self));
      break;
    case PROP_NUM_MESSAGES:
      g_value_set_ulong (value, stamp_conversation_item_get_num_messages (self));
      break;
    case PROP_LABELS:
      g_value_set_pointer (value, stamp_conversation_item_get_labels (self));
      break;
    case PROP_ANSWERED:
      g_value_set_boolean (value, stamp_conversation_item_get_answered (self));
      break;
    case PROP_FORWARDED:
      g_value_set_boolean (value, stamp_conversation_item_get_forwarded (self));
      break;
  }
}

static gint64
get_newest_timestamp (CamelFolderThreadNode *node,
                      gint64                 highest)
{
  const CamelMessageInfo *message;
  gint64 time = highest;

  if (!node)
    return time;

  message = camel_folder_thread_node_get_item (node);
  if (message) {
    gint64 message_time = camel_message_info_get_date_received (message);

    /* In case it is 0, it was an outgoing mail so switch to sent date */
    if (message_time == 0)
      message_time = camel_message_info_get_date_sent (message);

    time = MAX (time, message_time);
  }

  for (CamelFolderThreadNode *child = camel_folder_thread_node_get_child (node); child; child = camel_folder_thread_node_get_next (child)) {
    time = get_newest_timestamp (child, time);
  }

  return time;
}

static gboolean
is_important (StampConversationItem *self)
{
  const CamelMessageInfo *message = camel_folder_thread_node_get_item (self->thread_node);
  gboolean important = FALSE;

  if (message) {
    const CamelNamedFlags *flags = camel_message_info_get_user_flags (message);
    guint len = camel_named_flags_get_length (flags);

    for (guint idx = 0; idx < len; idx++) {
      if (g_strcmp0 (camel_named_flags_get (flags, idx), "$Labelimportant") == 0) {
        important = TRUE;
        break;
      }
    }
  }

  return important;
}

static void
stamp_conversation_item_set_property (GObject      *object,
                                      guint         property_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  StampConversationItem *self = STAMP_CONVERSATION_ITEM (object);

  switch ((StampConversationItemProps)property_id) {
    case PROP_THREAD:
      g_set_object (&self->thread, g_value_get_object (value));
      break;
    case PROP_THREAD_NODE:
      self->thread_node = g_value_get_pointer (value);
      self->timestamp = get_newest_timestamp (self->thread_node, -1);
      self->unread = !has_thread_flag_all (self->thread_node, CAMEL_MESSAGE_SEEN);
      self->flagged = has_thread_flag_one (self->thread_node, CAMEL_MESSAGE_FLAGGED);
      self->important = is_important (self);

      if (self->thread_node) {
        const CamelMessageInfo *message;

        message = camel_folder_thread_node_get_item (self->thread_node);
        self->uid = g_strdup (camel_message_info_get_uid (message));
        self->uid_number = self->uid ? atoi (self->uid) : 0;
      }

      break;
    case PROP_SERVICE_UID:
      self->service_uid = g_strdup (g_value_get_string (value));
      break;
    case PROP_FROM:
    case PROP_SUBJECT:
    case PROP_FLAGGED:
    case PROP_UNREAD:
    case PROP_PREVIEW:
    case PROP_HAS_ATTACHMENT:
    case PROP_HAS_CALENDAR:
    case PROP_IMPORTANT:
    case PROP_DATE:
    case PROP_NUM_MESSAGES:
    case PROP_LABELS:
    case PROP_ANSWERED:
    case PROP_FORWARDED:
      break;
  }
}

static void
stamp_conversation_item_dispose (GObject *object)
{
  StampConversationItem *self = STAMP_CONVERSATION_ITEM (object);

  g_clear_object (&self->thread);
  g_clear_pointer (&self->senders, g_free);
  g_clear_pointer (&self->subject, g_free);
  g_clear_pointer (&self->sender, g_free);
  g_clear_pointer (&self->service_uid, g_free);
  g_clear_pointer (&self->uid, g_free);
  g_clear_pointer (&self->preview, g_free);

  G_OBJECT_CLASS (stamp_conversation_item_parent_class)->dispose (object);
}

static void
stamp_conversation_item_constructed (GObject *object)
{
  G_OBJECT_CLASS (stamp_conversation_item_parent_class)->constructed (object);
}

void
stamp_conversation_item_class_init (StampConversationItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = stamp_conversation_item_get_property;
  object_class->set_property = stamp_conversation_item_set_property;
  object_class->dispose = stamp_conversation_item_dispose;
  object_class->constructed = stamp_conversation_item_constructed;

  properties[PROP_THREAD] = g_param_spec_object ("thread",
                                                 NULL,
                                                 NULL,
                                                 CAMEL_TYPE_FOLDER_THREAD,
                                                 G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_THREAD_NODE] = g_param_spec_pointer ("thread-node",
                                                       NULL,
                                                       NULL,
                                                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_UNREAD] = g_param_spec_boolean ("unread",
                                                  NULL,
                                                  NULL,
                                                  FALSE,
                                                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_FLAGGED] = g_param_spec_boolean ("flagged",
                                                   NULL,
                                                   NULL,
                                                   FALSE,
                                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_SERVICE_UID] = g_param_spec_string ("service-uid",
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_SUBJECT] = g_param_spec_string ("subject",
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_FROM] = g_param_spec_string ("from",
                                               NULL,
                                               NULL,
                                               NULL,
                                               G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_PREVIEW] = g_param_spec_string ("preview",
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_HAS_ATTACHMENT] = g_param_spec_boolean ("has-attachment",
                                                          NULL,
                                                          NULL,
                                                          FALSE,
                                                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_HAS_CALENDAR] = g_param_spec_boolean ("has-calendar",
                                                        NULL,
                                                        NULL,
                                                        FALSE,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IMPORTANT] = g_param_spec_boolean ("important",
                                                     NULL,
                                                     NULL,
                                                     FALSE,
                                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_DATE] = g_param_spec_ulong ("date",
                                              NULL,
                                              NULL,
                                              0,
                                              G_MAXULONG,
                                              0,
                                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_NUM_MESSAGES] = g_param_spec_ulong ("num-messages",
                                                      NULL,
                                                      NULL,
                                                      0,
                                                      G_MAXULONG,
                                                      0,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_LABELS] = g_param_spec_pointer ("labels",
                                                  NULL,
                                                  NULL,
                                                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ANSWERED] = g_param_spec_boolean ("answered",
                                                    NULL,
                                                    NULL,
                                                    FALSE,
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_FORWARDED] = g_param_spec_boolean ("forwarded",
                                                     NULL,
                                                     NULL,
                                                     FALSE,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);
}

void
stamp_conversation_item_init (StampConversationItem *self)
{
}

StampConversationItem *
stamp_conversation_item_new (CamelFolderThread     *thread,
                             CamelFolderThreadNode *thread_node,
                             const gchar           *service_uid)
{
  return g_object_new (STAMP_TYPE_CONVERSATION_ITEM,
                       "thread", thread,
                       "thread-node", thread_node,
                       "service-uid", service_uid,
                       NULL);
}

CamelFolderThreadNode *
stamp_conversation_item_get_node (StampConversationItem *self)
{
  return self->thread_node;
}

const gchar *
stamp_conversation_item_get_from (StampConversationItem *self)
{
  CamelFolderThreadNode *current_node = self->thread_node;
  g_autoptr (GHashTable) senders = NULL;
  const gchar *ia_name;
  const gchar *ia_address;

  if (self->sender)
    return self->sender;

  senders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  while (current_node) {
    const CamelMessageInfo *message = camel_folder_thread_node_get_item (current_node);

    if (message) {
      g_autoptr (CamelInternetAddress) address = camel_internet_address_new ();

      if (camel_address_decode (CAMEL_ADDRESS (address), camel_message_info_get_from (message)) > 0) {
        const gchar *sender = NULL;
        gchar *tmp = NULL;

        camel_internet_address_get (address, 0, &ia_name, &ia_address);
        if (g_strcmp0 (ia_name, "") != 0) {
          sender = ia_name;
        } else {
          sender = ia_address;
        }

        if (strlen (sender) > 2 && sender[0] == '<' && sender[strlen (sender) - 1] == '>') {
          tmp = g_strndup (sender + 1, strlen (sender) - 2);
        } else {
          tmp = g_strdup (sender);
        }

        g_hash_table_add (senders, tmp);
      }
    }

    current_node = camel_folder_thread_node_get_child (current_node);
  }

  if (g_hash_table_size (senders) > 0) {
    gchar **keys = (char **)g_hash_table_get_keys_as_array (senders, NULL);

    self->sender = g_strjoinv (", ", (char **)keys);
    g_clear_pointer (&keys, g_free);
  } else
    self->sender = g_strdup (_("Unknown"));

  return self->sender;
}

gint64
stamp_conversation_item_get_date (StampConversationItem *self)
{
  return self->timestamp;
}

static guint
count_thread_messages (CamelFolderThreadNode *node)
{
  guint num = 1;

  for (CamelFolderThreadNode *child = camel_folder_thread_node_get_child (node); child; child = camel_folder_thread_node_get_next (child)) {
    num += count_thread_messages (child);
  }

  return num;
}

guint
stamp_conversation_item_get_num_messages (StampConversationItem *self)
{
  return count_thread_messages (self->thread_node);
}

/* Check whether all nodes are SEEN */
gboolean
stamp_conversation_item_get_unread (StampConversationItem *self)
{
  return self->unread;
}

void
stamp_conversation_item_set_unread (StampConversationItem *self,
                                    gboolean               unread)
{
  CamelMessageInfo *message;

  if (!self->thread_node)
    return;

  message = camel_folder_thread_node_get_item (self->thread_node);

  camel_message_info_set_flags (message, CAMEL_MESSAGE_SEEN, unread ? ~0 : 0);
}

/* Check if one node has an ATTACHMENT */
gboolean
stamp_conversation_item_has_attachment (StampConversationItem *self)
{
  return has_thread_flag_one (self->thread_node, CAMEL_MESSAGE_ATTACHMENTS);
}

/* Check if one node has been FLAGGED */
gboolean
stamp_conversation_item_get_flagged (StampConversationItem *self)
{
  return self->flagged;
}

void
stamp_conversation_item_set_flagged (StampConversationItem *self,
                                     gboolean               flagged)
{
  CamelMessageInfo *message;

  if (!self->thread_node)
    return;

  message = camel_folder_thread_node_get_item (self->thread_node);

  camel_message_info_set_flags (message, CAMEL_MESSAGE_FLAGGED, flagged ? ~0 : 0);
}

/* Check if last node has been answered */
gboolean
stamp_conversation_item_get_answered (StampConversationItem *self)
{
  return has_thread_flag_one (self->thread_node, CAMEL_MESSAGE_ANSWERED);
}

/* Check if a node has been forwarded */
gboolean
stamp_conversation_item_get_forwarded (StampConversationItem *self)
{
  return has_thread_flag_one (self->thread_node, CAMEL_MESSAGE_FORWARDED);
}

const gchar *
stamp_conversation_item_get_preview (StampConversationItem *self)
{
  if (!self->preview) {
    const gchar *preview = NULL;
    const CamelMessageInfo *info = NULL;

    for (CamelFolderThreadNode *child = camel_folder_thread_node_get_child (self->thread_node); child; child = camel_folder_thread_node_get_next (child)) {
      info = camel_folder_thread_node_get_item (child);
    }

    if (!info)
      info = camel_folder_thread_node_get_item (self->thread_node);

    preview = camel_message_info_get_preview (info);

    if (preview && strlen (preview) > 0)
      self->preview = g_strdup (preview);
  }

  return self->preview;
}

guint
stamp_conversation_item_get_timestamp (StampConversationItem *self)
{
  return self->timestamp;
}

gchar *
stamp_conversation_item_get_mail (StampConversationItem *self)
{
  CamelFolderThreadNode *current_node = self->thread_node;

  while (current_node) {
    const CamelMessageInfo *message = camel_folder_thread_node_get_item (current_node);

    if (message) {
      g_autoptr (CamelInternetAddress) address = camel_internet_address_new ();

      if (camel_address_decode (CAMEL_ADDRESS (address), camel_message_info_get_from (message)) > 0) {
        const gchar *ia_name;
        const gchar *ia_address;
        const gchar *sender;

        camel_internet_address_get (address, 0, &ia_name, &ia_address);
        sender = ia_address;

        return g_strdup (sender);
      }
    }

    current_node = camel_folder_thread_node_get_child (current_node);
  }

  return NULL;
}

gchar *
stamp_conversation_item_get_service_uid (StampConversationItem *self)
{
  return self->service_uid;
}

const gchar *
stamp_conversation_item_get_uid (StampConversationItem *self)
{
  return self->uid;
}

gint64
stamp_conversation_item_get_uid_number (StampConversationItem *self)
{
  return self->uid_number;
}

gboolean
stamp_conversation_item_get_calendar (StampConversationItem *self)
{
  const CamelMessageInfo *message;

  if (!self->thread_node)
    return FALSE;

  message = camel_folder_thread_node_get_item (self->thread_node);

  return camel_message_info_get_user_flag (message, "$has_cal");
}

GPtrArray *
stamp_conversation_item_get_labels (StampConversationItem *self)
{
  const CamelMessageInfo *message;
  const CamelNamedFlags *flags;
  GPtrArray *array;

  if (!self->thread_node)
    return FALSE;

  array = g_ptr_array_new_with_free_func (g_free);
  message = camel_folder_thread_node_get_item (self->thread_node);

  flags = camel_message_info_get_user_flags (message);

  for (gint idx = 0; idx < camel_named_flags_get_length (flags); idx++) {
    const gchar *name = camel_named_flags_get (flags, idx);

    if (g_strcmp0 (name, "$has_cal") != 0 && g_strcmp0 (name, "$Labelimportant") != 0 && !g_str_has_prefix (name, "X-")) {
      g_ptr_array_add (array, g_strdup (name));
    }
  }

  return array;
}

void
stamp_conversation_item_update (StampConversationItem *self,
                                CamelMessageInfo      *info)
{
  self->unread = !has_thread_flag_all (self->thread_node, CAMEL_MESSAGE_SEEN);
  self->flagged = has_thread_flag_one (self->thread_node, CAMEL_MESSAGE_FLAGGED);
  self->important = is_important (self);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_UNREAD]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FLAGGED]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ANSWERED]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FORWARDED]);
}

gboolean
stamp_conversation_item_is_important (StampConversationItem *self)
{
  return self->important;
}

void
stamp_conversation_item_set_hidden (StampConversationItem *self,
                                    gboolean               hidden)
{
  self->hidden = hidden;
}

gboolean
stamp_conversation_item_get_hidden (StampConversationItem *self)
{
  return self->hidden;
}

void
stamp_conversation_item_set_label (StampConversationItem *self,
                                   const gchar           *label,
                                   gboolean               state)
{
  CamelMessageInfo *info;

  if (!self->thread_node)
    return;

  info = camel_folder_thread_node_get_item (self->thread_node);
  camel_message_info_set_user_flag (info, label, state);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_LABELS]);
}
