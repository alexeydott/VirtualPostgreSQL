#include "vps_connection_pool.h"

#include <limits.h>
#include <string.h>

typedef struct VpsPoolSlot {
    void *connection;
    uint64_t lease_token;
    uint64_t last_used_ms;
    int in_use;
} VpsPoolSlot;

typedef struct VpsPoolWaiter {
    uint64_t ticket;
    int active;
} VpsPoolWaiter;

struct VpsConnectionPool {
    VpsAllocator allocator;
    const VpsPlatformOperations *platform;
    VpsLogger *logger;
    VpsConnectionPoolCallbacks callbacks;
    VpsPlatformMutex mutex;
    VpsPlatformCondition condition;
    VpsPoolSlot *slots;
    VpsPoolWaiter *waiters;
    unsigned char *identity;
    size_t identity_size;
    char fingerprint[VPS_CONNECTION_POOL_FINGERPRINT_BUFFER_SIZE];
    uint64_t credential_generation;
    uint64_t configuration_generation;
    uint64_t idle_validation_ms;
    uint64_t next_ticket;
    uint64_t next_token;
    uint32_t maximum_size;
    uint32_t maximum_waiters;
    uint32_t wait_slice_ms;
    uint32_t total;
    uint32_t active;
    uint32_t waiting;
    int read_only;
    int closed;
    int mutex_initialized;
    int condition_initialized;
};

static int vps_pool_config_valid(const VpsConnectionPoolConfig *config)
{
    size_t fingerprint_length;
    uint64_t required = VPS_PLATFORM_CAP_MONOTONIC_CLOCK |
                        VPS_PLATFORM_CAP_MUTEX | VPS_PLATFORM_CAP_CONDITION;

    if (config == NULL || !vps_allocator_is_valid(&config->allocator) ||
        vps_platform_validate_operations(config->platform, required) !=
            VPS_PLATFORM_OK ||
        config->callbacks.create == NULL ||
        config->callbacks.destroy == NULL || config->key.identity == NULL ||
        config->key.identity_size == 0U || config->key.fingerprint == NULL ||
        config->minimum_size > config->maximum_size ||
        config->maximum_size == 0U ||
        config->maximum_size > VPS_CONNECTION_POOL_MAX_SLOTS ||
        config->maximum_waiters == 0U ||
        config->maximum_waiters > VPS_CONNECTION_POOL_MAX_WAITERS ||
        config->wait_slice_ms == 0U ||
        config->wait_slice_ms > VPS_WAIT_MAX_SLICE_MS ||
        config->idle_validation_ms > VPS_CONNECTION_POOL_MAX_TIMER_MS ||
        (config->key.read_only != 0 && config->key.read_only != 1)) {
        return 0;
    }
    fingerprint_length = strlen(config->key.fingerprint);
    return fingerprint_length == VPS_CONNECTION_POOL_FINGERPRINT_LENGTH;
}

static int vps_pool_key_matches(const VpsConnectionPool *pool,
                                const VpsConnectionPoolKey *key)
{
    return key != NULL && key->identity != NULL &&
           key->identity_size == pool->identity_size &&
           memcmp(key->identity, pool->identity, pool->identity_size) == 0 &&
           key->credential_generation == pool->credential_generation &&
           key->configuration_generation == pool->configuration_generation &&
           key->read_only == pool->read_only;
}

static uint64_t vps_pool_next_nonzero(uint64_t *value)
{
    *value += UINT64_C(1);
    if (*value == 0U) {
        *value = UINT64_C(1);
    }
    return *value;
}

static void vps_pool_log(VpsConnectionPool *pool,
                         const char *operation,
                         VpsConnectionPoolResult result,
                         uint64_t duration_ms)
{
    VpsLogEvent event;
    const char *status = vps_connection_pool_result_name(result);
    if (pool->logger == NULL ||
        vps_log_event_init(&event, result == VPS_CONNECTION_POOL_OK
                                       ? VPS_LOG_LEVEL_DEBUG
                                       : VPS_LOG_LEVEL_WARN) != VPS_LOG_OK) {
        return;
    }
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                   strlen(operation));
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                   strlen(status));
    (void)vps_log_event_add_string(
        &event, VPS_LOG_FIELD_CONNECTION_FINGERPRINT, pool->fingerprint,
        VPS_CONNECTION_POOL_FINGERPRINT_LENGTH);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_GENERATION,
                                   pool->credential_generation);
    (void)vps_log_event_add_uint64(&event,
                                   VPS_LOG_FIELD_CONFIGURATION_GENERATION,
                                   pool->configuration_generation);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_POOL_ACTIVE,
                                   pool->active);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_POOL_IDLE,
                                   pool->total - pool->active);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_POOL_WAITING,
                                   pool->waiting);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_DURATION_MS,
                                   duration_ms);
    vps_logger_emit(pool->logger, &event);
}

static void vps_pool_destroy_slot(VpsConnectionPool *pool, uint32_t index)
{
    VpsPoolSlot *slot = &pool->slots[index];
    if (slot->connection != NULL) {
        pool->callbacks.destroy(pool->callbacks.context, slot->connection);
        slot->connection = NULL;
        slot->lease_token = 0U;
        slot->last_used_ms = 0U;
        slot->in_use = 0;
        pool->total -= 1U;
    }
}

static VpsConnectionPoolResult vps_pool_create_slot(VpsConnectionPool *pool,
                                                     uint32_t *slot_index)
{
    uint32_t index;
    void *connection = NULL;
    VpsConnectionPoolResult result;

    for (index = 0U; index < pool->maximum_size; ++index) {
        if (pool->slots[index].connection == NULL) {
            result = pool->callbacks.create(pool->callbacks.context,
                                            &connection);
            if (result != VPS_CONNECTION_POOL_OK || connection == NULL) {
                if (connection != NULL) {
                    pool->callbacks.destroy(pool->callbacks.context,
                                            connection);
                }
                return VPS_CONNECTION_POOL_CREATE_FAILED;
            }
            pool->slots[index].connection = connection;
            pool->slots[index].last_used_ms = 0U;
            pool->total += 1U;
            *slot_index = index;
            return VPS_CONNECTION_POOL_OK;
        }
    }
    return VPS_CONNECTION_POOL_BUSY;
}

static uint64_t vps_pool_first_ticket(const VpsConnectionPool *pool)
{
    uint64_t first = UINT64_MAX;
    uint32_t index;
    for (index = 0U; index < pool->maximum_waiters; ++index) {
        if (pool->waiters[index].active && pool->waiters[index].ticket < first) {
            first = pool->waiters[index].ticket;
        }
    }
    return first;
}

static int vps_pool_waiter_add(VpsConnectionPool *pool,
                               uint32_t *waiter_index,
                               uint64_t *ticket)
{
    uint32_t index;
    for (index = 0U; index < pool->maximum_waiters; ++index) {
        if (!pool->waiters[index].active) {
            pool->waiters[index].active = 1;
            pool->waiters[index].ticket = vps_pool_next_nonzero(
                &pool->next_ticket);
            pool->waiting += 1U;
            *waiter_index = index;
            *ticket = pool->waiters[index].ticket;
            return 1;
        }
    }
    return 0;
}

static void vps_pool_waiter_remove(VpsConnectionPool *pool,
                                   uint32_t waiter_index)
{
    if (waiter_index < pool->maximum_waiters &&
        pool->waiters[waiter_index].active) {
        pool->waiters[waiter_index].active = 0;
        pool->waiters[waiter_index].ticket = 0U;
        pool->waiting -= 1U;
    }
}

static VpsConnectionPoolResult vps_pool_take_available(
    VpsConnectionPool *pool, uint64_t now_ms, uint32_t *slot_index)
{
    uint32_t index;
    VpsConnectionPoolResult result;

    for (index = 0U; index < pool->maximum_size; ++index) {
        VpsPoolSlot *slot = &pool->slots[index];
        if (slot->connection == NULL || slot->in_use) {
            continue;
        }
        if (pool->callbacks.validate != NULL &&
            (pool->idle_validation_ms == 0U || slot->last_used_ms == 0U ||
             now_ms - slot->last_used_ms >= pool->idle_validation_ms)) {
            result = pool->callbacks.validate(pool->callbacks.context,
                                              slot->connection);
            if (result != VPS_CONNECTION_POOL_OK) {
                vps_pool_destroy_slot(pool, index);
                continue;
            }
        }
        *slot_index = index;
        return VPS_CONNECTION_POOL_OK;
    }
    if (pool->total < pool->maximum_size) {
        return vps_pool_create_slot(pool, slot_index);
    }
    return VPS_CONNECTION_POOL_BUSY;
}

static void vps_pool_fill_lease(VpsConnectionPool *pool,
                                uint32_t slot_index,
                                VpsConnectionLease *lease)
{
    VpsPoolSlot *slot = &pool->slots[slot_index];
    slot->in_use = 1;
    slot->lease_token = vps_pool_next_nonzero(&pool->next_token);
    pool->active += 1U;
    lease->pool = pool;
    lease->connection = slot->connection;
    lease->slot_index = slot_index;
    lease->token = slot->lease_token;
}

VpsConnectionPoolResult vps_connection_pool_create(
    const VpsConnectionPoolConfig *config, VpsConnectionPool **pool_out)
{
    VpsConnectionPool *pool = NULL;
    size_t slots_size;
    size_t waiters_size;
    uint32_t index;
    VpsConnectionPoolResult result = VPS_CONNECTION_POOL_OK;

    if (pool_out == NULL || *pool_out != NULL || !vps_pool_config_valid(config) ||
        vps_size_multiply(config->maximum_size, sizeof(VpsPoolSlot),
                          &slots_size) != VPS_MEMORY_OK ||
        vps_size_multiply(config->maximum_waiters, sizeof(VpsPoolWaiter),
                          &waiters_size) != VPS_MEMORY_OK) {
        return VPS_CONNECTION_POOL_INVALID_ARGUMENT;
    }
    if (vps_memory_allocate(&config->allocator, sizeof(*pool),
                            (void **)&pool) != VPS_MEMORY_OK) {
        return VPS_CONNECTION_POOL_OUT_OF_MEMORY;
    }
    (void)memset(pool, 0, sizeof(*pool));
    pool->allocator = config->allocator;
    pool->platform = config->platform;
    pool->logger = config->logger;
    pool->callbacks = config->callbacks;
    pool->maximum_size = config->maximum_size;
    pool->maximum_waiters = config->maximum_waiters;
    pool->wait_slice_ms = config->wait_slice_ms;
    pool->idle_validation_ms = config->idle_validation_ms;
    pool->identity_size = config->key.identity_size;
    pool->credential_generation = config->key.credential_generation;
    pool->configuration_generation = config->key.configuration_generation;
    pool->read_only = config->key.read_only;
    (void)memcpy(pool->fingerprint, config->key.fingerprint,
                 VPS_CONNECTION_POOL_FINGERPRINT_BUFFER_SIZE);
    if (vps_memory_allocate(&pool->allocator, slots_size,
                            (void **)&pool->slots) != VPS_MEMORY_OK ||
        vps_memory_allocate(&pool->allocator, waiters_size,
                            (void **)&pool->waiters) != VPS_MEMORY_OK ||
        vps_memory_allocate(&pool->allocator, pool->identity_size,
                            (void **)&pool->identity) != VPS_MEMORY_OK) {
        result = VPS_CONNECTION_POOL_OUT_OF_MEMORY;
        goto fail;
    }
    (void)memset(pool->slots, 0, slots_size);
    (void)memset(pool->waiters, 0, waiters_size);
    (void)memcpy(pool->identity, config->key.identity, pool->identity_size);
    if (vps_platform_mutex_init(pool->platform, &pool->mutex) !=
        VPS_PLATFORM_OK) {
        result = VPS_CONNECTION_POOL_PLATFORM_ERROR;
        goto fail;
    }
    pool->mutex_initialized = 1;
    if (vps_platform_condition_init(pool->platform, &pool->condition) !=
        VPS_PLATFORM_OK) {
        result = VPS_CONNECTION_POOL_PLATFORM_ERROR;
        goto fail;
    }
    pool->condition_initialized = 1;
    for (index = 0U; index < config->minimum_size; ++index) {
        uint32_t slot_index = 0U;
        result = vps_pool_create_slot(pool, &slot_index);
        if (result != VPS_CONNECTION_POOL_OK) {
            goto fail;
        }
    }
    *pool_out = pool;
    return VPS_CONNECTION_POOL_OK;

fail:
    if (pool != NULL) {
        while (pool->total > 0U) {
            for (index = 0U; index < pool->maximum_size; ++index) {
                if (pool->slots != NULL &&
                    pool->slots[index].connection != NULL) {
                    vps_pool_destroy_slot(pool, index);
                }
            }
        }
        if (pool->condition_initialized) {
            (void)vps_platform_condition_destroy(pool->platform,
                                                 &pool->condition);
        }
        if (pool->mutex_initialized) {
            (void)vps_platform_mutex_destroy(pool->platform, &pool->mutex);
        }
        vps_memory_release(&pool->allocator, (void **)&pool->identity,
                           pool->identity_size);
        vps_memory_release(&pool->allocator, (void **)&pool->waiters,
                           waiters_size);
        vps_memory_release(&pool->allocator, (void **)&pool->slots, slots_size);
        vps_memory_release(&pool->allocator, (void **)&pool, sizeof(*pool));
    }
    return result;
}

VpsConnectionPoolResult vps_connection_pool_acquire(
    VpsConnectionPool *pool,
    const VpsConnectionPoolKey *key,
    uint64_t timeout_ms,
    VpsInterruptProbe interrupt_probe,
    void *interrupt_context,
    VpsConnectionLease *lease)
{
    VpsDeadline deadline;
    uint32_t waiter_index = UINT32_MAX;
    uint64_t ticket = 0U;
    VpsConnectionPoolResult result = VPS_CONNECTION_POOL_BUSY;

    if (pool == NULL || lease == NULL || lease->pool != NULL ||
        timeout_ms > VPS_CONNECTION_POOL_MAX_TIMER_MS) {
        return VPS_CONNECTION_POOL_INVALID_ARGUMENT;
    }
    if (!vps_pool_key_matches(pool, key)) {
        return VPS_CONNECTION_POOL_KEY_MISMATCH;
    }
    if (vps_deadline_start(pool->platform, timeout_ms, &deadline) !=
        VPS_DEADLINE_OK ||
        vps_platform_mutex_lock(pool->platform, &pool->mutex) !=
            VPS_PLATFORM_OK) {
        return VPS_CONNECTION_POOL_PLATFORM_ERROR;
    }
    for (;;) {
        uint64_t now_ms = 0U;
        uint64_t remaining_ms = 0U;
        int expired = 0;
        uint32_t slot_index = 0U;
        VpsPlatformStatus wait_status;

        if (pool->closed) {
            result = VPS_CONNECTION_POOL_CLOSED;
            break;
        }
        if (interrupt_probe != NULL) {
            VpsInterruptProbeResult probe = interrupt_probe(interrupt_context);
            if (probe != VPS_INTERRUPT_CONTINUE) {
                result = probe == VPS_INTERRUPT_REQUESTED
                             ? VPS_CONNECTION_POOL_INTERRUPTED
                             : VPS_CONNECTION_POOL_PLATFORM_ERROR;
                break;
            }
        }
        if (vps_platform_monotonic_now_ms(pool->platform, &now_ms) !=
            VPS_PLATFORM_OK) {
            result = VPS_CONNECTION_POOL_PLATFORM_ERROR;
            break;
        }
        if ((ticket == 0U && pool->waiting == 0U) ||
            (ticket != 0U && ticket == vps_pool_first_ticket(pool))) {
            result = vps_pool_take_available(pool, now_ms, &slot_index);
            if (result == VPS_CONNECTION_POOL_OK) {
                vps_pool_waiter_remove(pool, waiter_index);
                vps_pool_fill_lease(pool, slot_index, lease);
                (void)vps_platform_condition_broadcast(pool->platform,
                                                       &pool->condition);
                break;
            }
            if (result != VPS_CONNECTION_POOL_BUSY) {
                break;
            }
        }
        if (ticket == 0U &&
            !vps_pool_waiter_add(pool, &waiter_index, &ticket)) {
            result = VPS_CONNECTION_POOL_BUSY;
            break;
        }
        if (vps_deadline_remaining_at(&deadline, now_ms, &remaining_ms,
                                      &expired) != VPS_DEADLINE_OK) {
            result = VPS_CONNECTION_POOL_PLATFORM_ERROR;
            break;
        }
        if (expired) {
            result = VPS_CONNECTION_POOL_BUSY;
            break;
        }
        if (remaining_ms > pool->wait_slice_ms) {
            remaining_ms = pool->wait_slice_ms;
        }
        wait_status = vps_platform_condition_wait(
            pool->platform, &pool->condition, &pool->mutex,
            (uint32_t)remaining_ms);
        if (wait_status != VPS_PLATFORM_OK &&
            wait_status != VPS_PLATFORM_TIMEOUT) {
            result = VPS_CONNECTION_POOL_PLATFORM_ERROR;
            break;
        }
    }
    vps_pool_waiter_remove(pool, waiter_index);
    {
        uint64_t finished_ms = deadline.started_at_ms;
        (void)vps_platform_monotonic_now_ms(pool->platform, &finished_ms);
        vps_pool_log(pool, "pool_acquire", result,
                     finished_ms >= deadline.started_at_ms
                         ? finished_ms - deadline.started_at_ms
                         : 0U);
    }
    (void)vps_platform_mutex_unlock(pool->platform, &pool->mutex);
    return result;
}

VpsConnectionPoolResult vps_connection_lease_release(
    VpsConnectionLease *lease, VpsConnectionLeaseDisposition disposition)
{
    VpsConnectionPool *pool;
    VpsPoolSlot *slot;
    uint64_t now_ms = 0U;
    VpsConnectionPoolResult result = VPS_CONNECTION_POOL_OK;

    if (lease == NULL || lease->pool == NULL ||
        (disposition != VPS_CONNECTION_LEASE_CLEAN &&
         disposition != VPS_CONNECTION_LEASE_DIRTY)) {
        return VPS_CONNECTION_POOL_STALE_LEASE;
    }
    pool = lease->pool;
    if (vps_platform_mutex_lock(pool->platform, &pool->mutex) !=
        VPS_PLATFORM_OK) {
        return VPS_CONNECTION_POOL_PLATFORM_ERROR;
    }
    if (lease->slot_index >= pool->maximum_size) {
        result = VPS_CONNECTION_POOL_STALE_LEASE;
        goto unlock;
    }
    slot = &pool->slots[lease->slot_index];
    if (!slot->in_use || slot->connection != lease->connection ||
        slot->lease_token != lease->token) {
        result = VPS_CONNECTION_POOL_STALE_LEASE;
        goto unlock;
    }
    slot->in_use = 0;
    slot->lease_token = 0U;
    pool->active -= 1U;
    if (pool->closed || disposition == VPS_CONNECTION_LEASE_DIRTY) {
        vps_pool_destroy_slot(pool, lease->slot_index);
    } else if (pool->callbacks.reset != NULL &&
               pool->callbacks.reset(pool->callbacks.context,
                                     slot->connection) !=
                   VPS_CONNECTION_POOL_OK) {
        vps_pool_destroy_slot(pool, lease->slot_index);
        result = VPS_CONNECTION_POOL_RESET_FAILED;
    } else if (vps_platform_monotonic_now_ms(pool->platform, &now_ms) !=
               VPS_PLATFORM_OK) {
        vps_pool_destroy_slot(pool, lease->slot_index);
        result = VPS_CONNECTION_POOL_PLATFORM_ERROR;
    } else {
        slot->last_used_ms = now_ms;
    }
    lease->pool = NULL;
    lease->connection = NULL;
    lease->token = 0U;
    lease->slot_index = 0U;
    (void)vps_platform_condition_broadcast(pool->platform, &pool->condition);
    vps_pool_log(pool, "pool_release", result, 0U);
unlock:
    (void)vps_platform_mutex_unlock(pool->platform, &pool->mutex);
    return result;
}

VpsConnectionPoolResult vps_connection_pool_stats(
    VpsConnectionPool *pool, VpsConnectionPoolStats *stats)
{
    if (pool == NULL || stats == NULL ||
        vps_platform_mutex_lock(pool->platform, &pool->mutex) !=
            VPS_PLATFORM_OK) {
        return VPS_CONNECTION_POOL_INVALID_ARGUMENT;
    }
    stats->total = pool->total;
    stats->active = pool->active;
    stats->idle = pool->total - pool->active;
    stats->waiting = pool->waiting;
    stats->closed = pool->closed;
    (void)vps_platform_mutex_unlock(pool->platform, &pool->mutex);
    return VPS_CONNECTION_POOL_OK;
}

VpsConnectionPoolResult vps_connection_pool_close(VpsConnectionPool *pool)
{
    uint32_t index;
    if (pool == NULL ||
        vps_platform_mutex_lock(pool->platform, &pool->mutex) !=
            VPS_PLATFORM_OK) {
        return VPS_CONNECTION_POOL_INVALID_ARGUMENT;
    }
    pool->closed = 1;
    for (index = 0U; index < pool->maximum_size; ++index) {
        if (!pool->slots[index].in_use) {
            vps_pool_destroy_slot(pool, index);
        }
    }
    (void)vps_platform_condition_broadcast(pool->platform, &pool->condition);
    (void)vps_platform_mutex_unlock(pool->platform, &pool->mutex);
    return VPS_CONNECTION_POOL_OK;
}

VpsConnectionPoolResult vps_connection_pool_destroy(VpsConnectionPool **pool_ptr)
{
    VpsConnectionPool *pool;
    VpsAllocator allocator;
    size_t slots_size;
    size_t waiters_size;
    int busy;

    if (pool_ptr == NULL || *pool_ptr == NULL) {
        return VPS_CONNECTION_POOL_INVALID_ARGUMENT;
    }
    pool = *pool_ptr;
    if (vps_connection_pool_close(pool) != VPS_CONNECTION_POOL_OK ||
        vps_platform_mutex_lock(pool->platform, &pool->mutex) !=
            VPS_PLATFORM_OK) {
        return VPS_CONNECTION_POOL_PLATFORM_ERROR;
    }
    busy = pool->active != 0U || pool->waiting != 0U;
    (void)vps_platform_mutex_unlock(pool->platform, &pool->mutex);
    if (busy) {
        return VPS_CONNECTION_POOL_BUSY;
    }
    allocator = pool->allocator;
    slots_size = (size_t)pool->maximum_size * sizeof(VpsPoolSlot);
    waiters_size = (size_t)pool->maximum_waiters * sizeof(VpsPoolWaiter);
    (void)vps_platform_condition_destroy(pool->platform, &pool->condition);
    (void)vps_platform_mutex_destroy(pool->platform, &pool->mutex);
    vps_memory_release(&allocator, (void **)&pool->identity,
                       pool->identity_size);
    vps_memory_release(&allocator, (void **)&pool->waiters, waiters_size);
    vps_memory_release(&allocator, (void **)&pool->slots, slots_size);
    vps_memory_release(&allocator, (void **)pool_ptr, sizeof(*pool));
    return VPS_CONNECTION_POOL_OK;
}

const char *vps_connection_pool_result_name(VpsConnectionPoolResult result)
{
    static const char *const names[] = {
        "ok", "invalid_argument", "out_of_memory", "create_failed",
        "validate_failed", "reset_failed", "busy", "interrupted", "closed",
        "key_mismatch", "stale_lease", "platform_error"};
    if ((unsigned int)result >= sizeof(names) / sizeof(names[0])) {
        return "unknown";
    }
    return names[(unsigned int)result];
}
