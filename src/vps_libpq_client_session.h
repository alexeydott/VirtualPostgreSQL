#ifndef VPS_LIBPQ_CLIENT_SESSION_H
#define VPS_LIBPQ_CLIENT_SESSION_H

#include "vps_session.h"

/*
 * connection is a borrowed PGconn. This Stage 3 hook applies the portable
 * plan after connect/reset; Stage 4 consumes the same plan from its async
 * client state machine.
 */
VpsSessionResult vps_libpq_client_session_apply(void *connection,
                                                 const VpsSessionPlan *plan,
                                                 VpsSessionPhase phase);

#endif
