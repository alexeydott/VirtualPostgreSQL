#ifndef VPS_LIBPQ_CLIENT_CONNINFO_H
#define VPS_LIBPQ_CLIENT_CONNINFO_H

#include "vps_connection_string.h"

VpsConnectionStringResult vps_libpq_client_conninfo_parse(
    void *context,
    const char *conninfo,
    size_t conninfo_length,
    VpsConninfoConsumer consumer,
    void *consumer_context);

#endif
