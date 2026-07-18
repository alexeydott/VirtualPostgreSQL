#ifndef VPS_CONNECTION_POOL_H
#define VPS_CONNECTION_POOL_H

#include "vps_deadline.h"
#include "vps_memory.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_CONNECTION_POOL_MAX_SLOTS UINT32_C(1024)
#define VPS_CONNECTION_POOL_MAX_WAITERS UINT32_C(4096)
#define VPS_CONNECTION_POOL_MAX_TIMER_MS UINT64_C(86400000)
#define VPS_CONNECTION_POOL_FINGERPRINT_LENGTH 64U
#define VPS_CONNECTION_POOL_FINGERPRINT_BUFFER_SIZE 65U

typedef enum VpsConnectionPoolResult {
    VPS_CONNECTION_POOL_OK = 0,
    VPS_CONNECTION_POOL_INVALID_ARGUMENT = 1,
    VPS_CONNECTION_POOL_OUT_OF_MEMORY = 2,
    VPS_CONNECTION_POOL_CREATE_FAILED = 3,
    VPS_CONNECTION_POOL_VALIDATE_FAILED = 4,
    VPS_CONNECTION_POOL_RESET_FAILED = 5,
    VPS_CONNECTION_POOL_BUSY = 6,
    VPS_CONNECTION_POOL_INTERRUPTED = 7,
    VPS_CONNECTION_POOL_CLOSED = 8,
    VPS_CONNECTION_POOL_KEY_MISMATCH = 9,
    VPS_CONNECTION_POOL_STALE_LEASE = 10,
    VPS_CONNECTION_POOL_PLATFORM_ERROR = 11
} VpsConnectionPoolResult;

typedef enum VpsConnectionLeaseDisposition {
    VPS_CONNECTION_LEASE_CLEAN = 0,
    VPS_CONNECTION_LEASE_DIRTY = 1
} VpsConnectionLeaseDisposition;

/* Exact key bytes and fingerprint are borrowed only for the duration of a call. */
typedef struct VpsConnectionPoolKey {
    const unsigned char *identity;
    size_t identity_size;
    const char *fingerprint;
    uint64_t credential_generation;
    uint64_t configuration_generation;
    int read_only;
} VpsConnectionPoolKey;

typedef VpsConnectionPoolResult (*VpsPoolCreateConnection)(
    void *context, void **connection);
typedef VpsConnectionPoolResult (*VpsPoolConnectionOperation)(
    void *context, void *connection);
typedef void (*VpsPoolDestroyConnection)(void *context, void *connection);

typedef struct VpsConnectionPoolCallbacks {
    void *context;
    VpsPoolCreateConnection create;
    VpsPoolConnectionOperation validate;
    VpsPoolConnectionOperation reset;
    VpsPoolDestroyConnection destroy;
} VpsConnectionPoolCallbacks;

typedef struct VpsConnectionPoolConfig {
    VpsAllocator allocator;
    const VpsPlatformOperations *platform;
    VpsLogger *logger;
    VpsConnectionPoolKey key;
    VpsConnectionPoolCallbacks callbacks;
    uint32_t minimum_size;
    uint32_t maximum_size;
    uint32_t maximum_waiters;
    uint32_t wait_slice_ms;
    uint64_t idle_validation_ms;
} VpsConnectionPoolConfig;

typedef struct VpsConnectionPool VpsConnectionPool;

/* A lease exclusively borrows connection until exactly one successful release. */
typedef struct VpsConnectionLease {
    VpsConnectionPool *pool;
    void *connection;
    uint64_t token;
    uint32_t slot_index;
} VpsConnectionLease;

typedef struct VpsConnectionPoolStats {
    uint32_t total;
    uint32_t active;
    uint32_t idle;
    uint32_t waiting;
    int closed;
} VpsConnectionPoolStats;

VpsConnectionPoolResult vps_connection_pool_create(
    const VpsConnectionPoolConfig *config, VpsConnectionPool **pool);
VpsConnectionPoolResult vps_connection_pool_acquire(
    VpsConnectionPool *pool,
    const VpsConnectionPoolKey *key,
    uint64_t timeout_ms,
    VpsInterruptProbe interrupt_probe,
    void *interrupt_context,
    VpsConnectionLease *lease);
VpsConnectionPoolResult vps_connection_lease_release(
    VpsConnectionLease *lease, VpsConnectionLeaseDisposition disposition);
VpsConnectionPoolResult vps_connection_pool_stats(
    VpsConnectionPool *pool, VpsConnectionPoolStats *stats);
VpsConnectionPoolResult vps_connection_pool_close(VpsConnectionPool *pool);
/*
 * Caller must prevent new entries. destroy first closes the pool and returns
 * BUSY without freeing storage until all active leases and registered waiters
 * have left their calls.
 */
VpsConnectionPoolResult vps_connection_pool_destroy(
    VpsConnectionPool **pool);

const char *vps_connection_pool_result_name(VpsConnectionPoolResult result);

#endif
