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

#pragma once

#include <libecal/libecal.h>

G_BEGIN_DECLS

ICalComponent *
stamp_meeting_response_ref_event (ICalComponent *calendar);

const gchar *
stamp_meeting_response_get_email (const gchar *value);

ICalComponent *
stamp_meeting_response_new_reply (ICalComponent          *invitation,
                                  const gchar            *email,
                                  ICalParameterPartstat   response,
                                  GError                **error);

gboolean
stamp_meeting_response_wants_reply (ICalComponent *invitation,
                                    const gchar   *email);

gboolean
stamp_meeting_response_receive_sync (ECalClient     *client,
                                     ICalComponent  *reply,
                                     const gchar    *email,
                                     GCancellable   *cancellable,
                                     GError        **error);

ESource *
stamp_meeting_response_choose_source (GList        *candidates,
                                      const gchar  *existing_uid,
                                      const gchar  *default_uid,
                                      GError      **error);

ECalClient *
stamp_meeting_response_ref_client_sync (ESourceRegistry  *registry,
                                        const gchar      *account_uid,
                                        ESource          *auth_source,
                                        ICalComponent    *invitation,
                                        GCancellable     *cancellable,
                                        GError          **error);

ECalClient *
stamp_meeting_response_ref_existing_client_sync (ESourceRegistry  *registry,
                                                 const gchar      *account_uid,
                                                 ESource          *auth_source,
                                                 ICalComponent    *invitation,
                                                 ICalComponent   **out_stored,
                                                 GCancellable     *cancellable,
                                                 GError          **error);

ECalComponentId *
stamp_meeting_response_get_removal_id (ICalComponent  *invitation,
                                       ICalComponent  *stored,
                                       GError        **error);

gboolean
stamp_meeting_response_contains_occurrence (ICalComponent *invitation,
                                            ICalComponent *stored);

G_END_DECLS
