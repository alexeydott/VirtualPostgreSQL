#ifndef VPS_LIBPQ_CLIENT_TLS_H
#define VPS_LIBPQ_CLIENT_TLS_H

#include "vps_tls_policy.h"

/* connection is a borrowed PGconn hidden behind the client-port boundary. */
VpsTlsResult vps_libpq_client_tls_verify(void *connection,
                                         const VpsTlsPolicy *policy,
                                         VpsTlsOutcome *outcome,
                                         VpsLogger *logger);

#endif
