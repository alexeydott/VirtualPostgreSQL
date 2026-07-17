#include "vps_secure_memory.h"

#include <string.h>

#define VPS_SECURE_MEMORY_SMALL_LIMIT 64U
#define VPS_SECURE_MEMORY_MEDIUM_LIMIT 4096U

static const char *vps_secure_memory_size_class(size_t size)
{
    if (size == 0U) {
        return "empty";
    }
    if (size <= VPS_SECURE_MEMORY_SMALL_LIMIT) {
        return "small";
    }
    if (size <= VPS_SECURE_MEMORY_MEDIUM_LIMIT) {
        return "medium";
    }
    return "large";
}

static void vps_secure_memory_log(VpsLogger *logger,
                                  const char *phase,
                                  size_t size,
                                  const char *status,
                                  VpsLogLevel level)
{
    static const char operation[] = "secure_memory";
    const char *size_class = vps_secure_memory_size_class(size);
    VpsLogEvent event;

    if (logger == NULL || phase == NULL || status == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 strlen(phase)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_SIZE_CLASS, size_class,
                                 strlen(size_class)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) {
        return;
    }
    vps_logger_emit(logger, &event);
}

static VpsSecureMemoryResult vps_secure_memory_from_memory_result(
    VpsMemoryResult result)
{
    if (result == VPS_MEMORY_OK) {
        return VPS_SECURE_MEMORY_OK;
    }
    if (result == VPS_MEMORY_OUT_OF_MEMORY) {
        return VPS_SECURE_MEMORY_OUT_OF_MEMORY;
    }
    return VPS_SECURE_MEMORY_INVALID_ARGUMENT;
}

VpsSecureMemoryResult vps_sensitive_memory_init(
    VpsSensitiveMemory *sensitive_memory,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger)
{
    VpsMemoryResult memory_result;

    if (sensitive_memory == NULL ||
        vps_platform_validate_operations(
            operations, VPS_PLATFORM_CAP_SECURE_ZERO) != VPS_PLATFORM_OK) {
        return VPS_SECURE_MEMORY_INVALID_ARGUMENT;
    }
    memory_result = vps_owned_memory_init(&sensitive_memory->storage,
                                          allocator);
    if (memory_result != VPS_MEMORY_OK) {
        return vps_secure_memory_from_memory_result(memory_result);
    }
    sensitive_memory->operations = operations;
    sensitive_memory->logger = logger;
    sensitive_memory->initialized = 1;
    return VPS_SECURE_MEMORY_OK;
}

VpsSecureMemoryResult vps_sensitive_memory_allocate(
    VpsSensitiveMemory *sensitive_memory,
    size_t size)
{
    VpsMemoryResult memory_result;
    VpsSecureMemoryResult result;

    if (sensitive_memory == NULL || !sensitive_memory->initialized) {
        return VPS_SECURE_MEMORY_INVALID_ARGUMENT;
    }
    memory_result = vps_owned_memory_allocate(&sensitive_memory->storage,
                                              size);
    result = vps_secure_memory_from_memory_result(memory_result);
    vps_secure_memory_log(sensitive_memory->logger, "allocation", size,
                          vps_secure_memory_result_name(result),
                          result == VPS_SECURE_MEMORY_OK
                              ? VPS_LOG_LEVEL_DEBUG
                              : VPS_LOG_LEVEL_ERROR);
    return result;
}

VpsSecureMemoryResult vps_sensitive_memory_adopt(
    VpsSensitiveMemory *sensitive_memory,
    void *memory,
    size_t size)
{
    VpsMemoryResult memory_result;
    VpsSecureMemoryResult result;

    if (sensitive_memory == NULL || !sensitive_memory->initialized) {
        return VPS_SECURE_MEMORY_INVALID_ARGUMENT;
    }
    memory_result = vps_owned_memory_adopt(&sensitive_memory->storage, memory,
                                           size);
    result = vps_secure_memory_from_memory_result(memory_result);
    vps_secure_memory_log(sensitive_memory->logger, "adoption", size,
                          vps_secure_memory_result_name(result),
                          result == VPS_SECURE_MEMORY_OK
                              ? VPS_LOG_LEVEL_DEBUG
                              : VPS_LOG_LEVEL_ERROR);
    return result;
}

VpsSecureMemoryResult vps_sensitive_memory_release(
    VpsSensitiveMemory *sensitive_memory)
{
    VpsPlatformStatus platform_status;
    size_t size;

    if (sensitive_memory == NULL || !sensitive_memory->initialized) {
        return VPS_SECURE_MEMORY_INVALID_ARGUMENT;
    }
    size = sensitive_memory->storage.size;
    if (!sensitive_memory->storage.owned) {
        vps_secure_memory_log(sensitive_memory->logger, "cleanup", 0U,
                              "already_released", VPS_LOG_LEVEL_DEBUG);
        return VPS_SECURE_MEMORY_OK;
    }
    platform_status = vps_platform_secure_zero(
        sensitive_memory->operations, sensitive_memory->storage.memory, size);
    if (platform_status != VPS_PLATFORM_OK) {
        vps_secure_memory_log(sensitive_memory->logger, "cleanup", size,
                              "zero_failed", VPS_LOG_LEVEL_ERROR);
        return VPS_SECURE_MEMORY_ZERO_FAILED;
    }
    vps_owned_memory_release(&sensitive_memory->storage);
    vps_secure_memory_log(sensitive_memory->logger, "cleanup", size,
                          "released", VPS_LOG_LEVEL_DEBUG);
    return VPS_SECURE_MEMORY_OK;
}

void *vps_sensitive_memory_data(VpsSensitiveMemory *sensitive_memory)
{
    if (sensitive_memory == NULL || !sensitive_memory->initialized ||
        !sensitive_memory->storage.owned) {
        return NULL;
    }
    return sensitive_memory->storage.memory;
}

size_t vps_sensitive_memory_size(const VpsSensitiveMemory *sensitive_memory)
{
    if (sensitive_memory == NULL || !sensitive_memory->initialized ||
        !sensitive_memory->storage.owned) {
        return 0U;
    }
    return sensitive_memory->storage.size;
}

const char *vps_secure_memory_result_name(VpsSecureMemoryResult result)
{
    static const char *const names[] = {"ok", "invalid_argument",
                                        "out_of_memory", "zero_failed"};

    if (result < VPS_SECURE_MEMORY_OK ||
        result > VPS_SECURE_MEMORY_ZERO_FAILED) {
        return "unknown";
    }
    return names[(size_t)result];
}
