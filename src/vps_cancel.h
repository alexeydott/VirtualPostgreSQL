#ifndef VPS_CANCEL_H
#define VPS_CANCEL_H

#include "vps_deadline.h"
#include "vps_logging.h"
#include "vps_platform.h"

#include <stddef.h>
#include <stdint.h>

typedef enum VpsCancelRegistryResult {
    VPS_CANCEL_REGISTRY_OK = 0,
    VPS_CANCEL_REGISTRY_INVALID_ARGUMENT = 1,
    VPS_CANCEL_REGISTRY_PLATFORM_ERROR = 2,
    VPS_CANCEL_REGISTRY_INVALID_STATE = 3
} VpsCancelRegistryResult;

typedef struct VpsCancelRegistry VpsCancelRegistry;

typedef struct VpsCancelToken {
    VpsCancelRegistry *registry;
    struct VpsCancelToken *next;
    uint64_t cursor_id;
    uint64_t request_generation;
    int registered;
    int requested;
} VpsCancelToken;

struct VpsCancelRegistry {
    const VpsPlatformOperations *platform;
    VpsPlatformMutex mutex;
    VpsCancelToken *head;
    VpsLogger *logger;
    uint64_t generation;
    size_t token_count;
    int initialized;
    int closing;
};

VpsCancelRegistryResult vps_cancel_registry_init(
    VpsCancelRegistry *registry,
    const VpsPlatformOperations *platform,
    VpsLogger *logger);
VpsCancelRegistryResult vps_cancel_registry_cleanup(
    VpsCancelRegistry *registry);
VpsCancelRegistryResult vps_cancel_token_register(VpsCancelRegistry *registry,
                                                  VpsCancelToken *token,
                                                  uint64_t cursor_id);
VpsCancelRegistryResult vps_cancel_token_unregister(VpsCancelToken *token);
VpsCancelRegistryResult vps_cancel_registry_request_all(
    VpsCancelRegistry *registry,
    size_t *requested_count);
VpsInterruptProbeResult vps_cancel_token_probe(void *context);

#endif
