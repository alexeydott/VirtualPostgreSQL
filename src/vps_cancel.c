#include "vps_cancel.h"

#include <string.h>

static void vps_cancel_log(VpsCancelRegistry *registry,
                           const char *phase,
                           const char *status,
                           uint64_t count)
{
    VpsLogEvent event;
    if (registry == NULL || registry->logger == NULL || phase == NULL ||
        status == NULL ||
        vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, "cancel",
                                 sizeof("cancel") - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 strlen(phase)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PARTICIPANT_COUNT,
                                 count) != VPS_LOG_OK)
        return;
    vps_logger_emit(registry->logger, &event);
}

VpsCancelRegistryResult vps_cancel_registry_init(
    VpsCancelRegistry *registry,
    const VpsPlatformOperations *platform,
    VpsLogger *logger)
{
    if (registry == NULL || platform == NULL)
        return VPS_CANCEL_REGISTRY_INVALID_ARGUMENT;
    (void)memset(registry, 0, sizeof(*registry));
    registry->platform = platform;
    registry->logger = logger;
    if (vps_platform_mutex_init(platform, &registry->mutex) != VPS_PLATFORM_OK) {
        (void)memset(registry, 0, sizeof(*registry));
        return VPS_CANCEL_REGISTRY_PLATFORM_ERROR;
    }
    registry->initialized = 1;
    return VPS_CANCEL_REGISTRY_OK;
}

VpsCancelRegistryResult vps_cancel_registry_cleanup(
    VpsCancelRegistry *registry)
{
    VpsCancelToken *token;
    if (registry == NULL) return VPS_CANCEL_REGISTRY_INVALID_ARGUMENT;
    if (!registry->initialized) return VPS_CANCEL_REGISTRY_OK;
    if (vps_platform_mutex_lock(registry->platform, &registry->mutex) !=
        VPS_PLATFORM_OK)
        return VPS_CANCEL_REGISTRY_PLATFORM_ERROR;
    registry->closing = 1;
    token = registry->head;
    while (token != NULL) {
        VpsCancelToken *next = token->next;
        (void)memset(token, 0, sizeof(*token));
        token = next;
    }
    registry->head = NULL;
    registry->token_count = 0U;
    (void)vps_platform_mutex_unlock(registry->platform, &registry->mutex);
    if (vps_platform_mutex_destroy(registry->platform, &registry->mutex) !=
        VPS_PLATFORM_OK)
        return VPS_CANCEL_REGISTRY_PLATFORM_ERROR;
    (void)memset(registry, 0, sizeof(*registry));
    return VPS_CANCEL_REGISTRY_OK;
}

VpsCancelRegistryResult vps_cancel_token_register(VpsCancelRegistry *registry,
                                                  VpsCancelToken *token,
                                                  uint64_t cursor_id)
{
    VpsCancelRegistryResult result = VPS_CANCEL_REGISTRY_OK;
    if (registry == NULL || token == NULL || cursor_id == 0U ||
        !registry->initialized || token->registered)
        return VPS_CANCEL_REGISTRY_INVALID_ARGUMENT;
    if (vps_platform_mutex_lock(registry->platform, &registry->mutex) !=
        VPS_PLATFORM_OK)
        return VPS_CANCEL_REGISTRY_PLATFORM_ERROR;
    if (registry->closing || registry->token_count == SIZE_MAX) {
        result = VPS_CANCEL_REGISTRY_INVALID_STATE;
    } else {
        (void)memset(token, 0, sizeof(*token));
        token->registry = registry;
        token->cursor_id = cursor_id;
        token->next = registry->head;
        token->registered = 1;
        registry->head = token;
        registry->token_count += 1U;
    }
    (void)vps_platform_mutex_unlock(registry->platform, &registry->mutex);
    vps_cancel_log(registry, "register",
                   result == VPS_CANCEL_REGISTRY_OK ? "ok" : "failed",
                   (uint64_t)registry->token_count);
    return result;
}

VpsCancelRegistryResult vps_cancel_token_unregister(VpsCancelToken *token)
{
    VpsCancelRegistry *registry;
    VpsCancelToken **link;
    if (token == NULL) return VPS_CANCEL_REGISTRY_INVALID_ARGUMENT;
    if (!token->registered) return VPS_CANCEL_REGISTRY_OK;
    registry = token->registry;
    if (registry == NULL || !registry->initialized)
        return VPS_CANCEL_REGISTRY_INVALID_STATE;
    if (vps_platform_mutex_lock(registry->platform, &registry->mutex) !=
        VPS_PLATFORM_OK)
        return VPS_CANCEL_REGISTRY_PLATFORM_ERROR;
    link = &registry->head;
    while (*link != NULL && *link != token) link = &(*link)->next;
    if (*link != token) {
        (void)vps_platform_mutex_unlock(registry->platform, &registry->mutex);
        return VPS_CANCEL_REGISTRY_INVALID_STATE;
    }
    *link = token->next;
    if (registry->token_count != 0U) registry->token_count -= 1U;
    (void)memset(token, 0, sizeof(*token));
    (void)vps_platform_mutex_unlock(registry->platform, &registry->mutex);
    vps_cancel_log(registry, "unregister", "ok",
                   (uint64_t)registry->token_count);
    return VPS_CANCEL_REGISTRY_OK;
}

VpsCancelRegistryResult vps_cancel_registry_request_all(
    VpsCancelRegistry *registry,
    size_t *requested_count)
{
    VpsCancelToken *token;
    size_t count = 0U;
    if (registry == NULL || !registry->initialized)
        return VPS_CANCEL_REGISTRY_INVALID_ARGUMENT;
    if (vps_platform_mutex_lock(registry->platform, &registry->mutex) !=
        VPS_PLATFORM_OK)
        return VPS_CANCEL_REGISTRY_PLATFORM_ERROR;
    if (registry->closing || registry->generation == UINT64_MAX) {
        (void)vps_platform_mutex_unlock(registry->platform, &registry->mutex);
        return VPS_CANCEL_REGISTRY_INVALID_STATE;
    }
    registry->generation += 1U;
    for (token = registry->head; token != NULL; token = token->next) {
        token->requested = 1;
        token->request_generation = registry->generation;
        count += 1U;
    }
    (void)vps_platform_mutex_unlock(registry->platform, &registry->mutex);
    if (requested_count != NULL) *requested_count = count;
    vps_cancel_log(registry, "request", "ok", (uint64_t)count);
    return VPS_CANCEL_REGISTRY_OK;
}

VpsInterruptProbeResult vps_cancel_token_probe(void *context)
{
    VpsCancelToken *token = (VpsCancelToken *)context;
    VpsCancelRegistry *registry;
    int requested;
    if (token == NULL || !token->registered || token->registry == NULL)
        return VPS_INTERRUPT_PROBE_ERROR;
    registry = token->registry;
    if (vps_platform_mutex_lock(registry->platform, &registry->mutex) !=
        VPS_PLATFORM_OK)
        return VPS_INTERRUPT_PROBE_ERROR;
    requested = token->registered && token->requested;
    (void)vps_platform_mutex_unlock(registry->platform, &registry->mutex);
    return requested ? VPS_INTERRUPT_REQUESTED : VPS_INTERRUPT_CONTINUE;
}
