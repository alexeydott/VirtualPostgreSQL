#ifndef VPS_SPATIAL_CLIENT_H
#define VPS_SPATIAL_CLIENT_H

#include "vps_client.h"
#include "vps_spatial.h"

VpsSpatialResult vps_spatial_capabilities_load(
    VpsSpatialCapabilities *capabilities,
    VpsClientConnection *connection,
    const VpsAllocator *allocator,
    VpsLogger *logger,
    VpsError *error);

#endif
