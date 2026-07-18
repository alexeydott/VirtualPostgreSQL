#include "vps_connection_pool.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define VPS_POOL_TEST_WORKERS 8U
#define VPS_POOL_TEST_ITERATIONS 1000U

typedef struct FakeConnection {
    uint32_t id;
    volatile long owners;
} FakeConnection;

typedef struct FakeBackend {
    FakeConnection connections[16];
    uint32_t create_count;
    uint32_t destroy_count;
    uint32_t validate_count;
    uint32_t reset_count;
    uint32_t fail_create_at;
    int fail_validate;
    int fail_reset;
    volatile long duplicate_ownership;
} FakeBackend;

typedef struct WorkerContext {
    VpsConnectionPool *pool;
    VpsConnectionPoolKey key;
    volatile long failures;
} WorkerContext;

typedef struct WaitingContext {
    VpsConnectionPool *pool;
    VpsConnectionPoolKey key;
    VpsConnectionPoolResult result;
    volatile long *sequence;
    long acquired_order;
} WaitingContext;

static int expect_true(int condition, const char *name)
{
    if (!condition) {
        (void)fprintf(stderr, "pool_test failure=%s\n", name);
        return 0;
    }
    return 1;
}

static VpsConnectionPoolResult fake_create(void *context, void **connection)
{
    FakeBackend *backend = (FakeBackend *)context;
    uint32_t index = backend->create_count;
    backend->create_count += 1U;
    if ((backend->fail_create_at != 0U &&
         backend->create_count == backend->fail_create_at) ||
        index >= 16U) {
        return VPS_CONNECTION_POOL_CREATE_FAILED;
    }
    backend->connections[index].id = index + 1U;
    backend->connections[index].owners = 0;
    *connection = &backend->connections[index];
    return VPS_CONNECTION_POOL_OK;
}

static VpsConnectionPoolResult fake_validate(void *context, void *connection)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)connection;
    backend->validate_count += 1U;
    return backend->fail_validate ? VPS_CONNECTION_POOL_VALIDATE_FAILED
                                  : VPS_CONNECTION_POOL_OK;
}

static VpsConnectionPoolResult fake_reset(void *context, void *connection)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)connection;
    backend->reset_count += 1U;
    return backend->fail_reset ? VPS_CONNECTION_POOL_RESET_FAILED
                               : VPS_CONNECTION_POOL_OK;
}

static void fake_destroy(void *context, void *connection)
{
    FakeBackend *backend = (FakeBackend *)context;
    FakeConnection *fake = (FakeConnection *)connection;
    fake->owners = 0;
    backend->destroy_count += 1U;
}

static VpsConnectionPoolConfig make_config(VpsAllocator allocator,
                                            FakeBackend *backend,
                                            const unsigned char *identity,
                                            uint32_t minimum,
                                            uint32_t maximum)
{
    static const char fingerprint[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    VpsConnectionPoolConfig config;
    (void)memset(&config, 0, sizeof(config));
    config.allocator = allocator;
    config.platform = vps_platform_current_operations();
    config.key.identity = identity;
    config.key.identity_size = 4U;
    config.key.fingerprint = fingerprint;
    config.key.credential_generation = 7U;
    config.key.configuration_generation = 11U;
    config.key.read_only = 1;
    config.callbacks.context = backend;
    config.callbacks.create = fake_create;
    config.callbacks.validate = fake_validate;
    config.callbacks.reset = fake_reset;
    config.callbacks.destroy = fake_destroy;
    config.minimum_size = minimum;
    config.maximum_size = maximum;
    config.maximum_waiters = 32U;
    config.wait_slice_ms = 5U;
    config.idle_validation_ms = 0U;
    return config;
}

static VpsInterruptProbeResult interrupt_requested(void *context)
{
    (void)context;
    return VPS_INTERRUPT_REQUESTED;
}

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID parameter)
{
    WorkerContext *worker = (WorkerContext *)parameter;
    uint32_t iteration;
    for (iteration = 0U; iteration < VPS_POOL_TEST_ITERATIONS; ++iteration) {
        VpsConnectionLease lease = {0};
        FakeConnection *connection;
        if (vps_connection_pool_acquire(worker->pool, &worker->key, 5000U,
                                        NULL, NULL, &lease) !=
            VPS_CONNECTION_POOL_OK) {
            (void)InterlockedIncrement(&worker->failures);
            continue;
        }
        connection = (FakeConnection *)lease.connection;
        if (InterlockedIncrement(&connection->owners) != 1) {
            (void)InterlockedIncrement(&worker->failures);
        }
        SwitchToThread();
        if (InterlockedDecrement(&connection->owners) != 0) {
            (void)InterlockedIncrement(&worker->failures);
        }
        if (vps_connection_lease_release(&lease,
                                         VPS_CONNECTION_LEASE_CLEAN) !=
            VPS_CONNECTION_POOL_OK) {
            (void)InterlockedIncrement(&worker->failures);
        }
    }
    return 0U;
}

static DWORD WINAPI waiting_main(LPVOID parameter)
{
    WaitingContext *waiting = (WaitingContext *)parameter;
    VpsConnectionLease lease = {0};
    waiting->result = vps_connection_pool_acquire(
        waiting->pool, &waiting->key, 5000U, NULL, NULL, &lease);
    if (waiting->result == VPS_CONNECTION_POOL_OK) {
        if (waiting->sequence != NULL) {
            waiting->acquired_order = InterlockedIncrement(waiting->sequence);
        }
        (void)vps_connection_lease_release(&lease,
                                           VPS_CONNECTION_LEASE_CLEAN);
    }
    return 0U;
}
#endif

int main(void)
{
    static const unsigned char identity[] = {'v', 'p', 's', '1'};
    static const unsigned char other_identity[] = {'v', 'p', 's', '2'};
    VpsAllocator allocator;
    FakeBackend backend;
    VpsConnectionPoolConfig config;
    VpsConnectionPool *pool = NULL;
    VpsConnectionPoolStats stats;
    VpsConnectionLease first = {0};
    VpsConnectionLease second = {0};
    VpsConnectionLease third = {0};
    VpsConnectionPoolKey wrong_key;
    int passed = 1;

    (void)memset(&backend, 0, sizeof(backend));
    passed &= expect_true(vps_allocator_system(&allocator) == VPS_MEMORY_OK,
                          "allocator");
    config = make_config(allocator, &backend, identity, 1U, 2U);
    passed &= expect_true(vps_connection_pool_create(&config, &pool) ==
                              VPS_CONNECTION_POOL_OK,
                          "create");
    passed &= expect_true(vps_connection_pool_acquire(
                              pool, &config.key, 100U, NULL, NULL, &first) ==
                              VPS_CONNECTION_POOL_OK,
                          "acquire_first");
    passed &= expect_true(vps_connection_pool_acquire(
                              pool, &config.key, 100U, NULL, NULL, &second) ==
                              VPS_CONNECTION_POOL_OK,
                          "acquire_second");
    passed &= expect_true(vps_connection_pool_acquire(
                              pool, &config.key, 0U, NULL, NULL, &third) ==
                              VPS_CONNECTION_POOL_BUSY,
                          "bounded_max_plus_one");
    passed &= expect_true(vps_connection_pool_acquire(
                              pool, &config.key, 50U, interrupt_requested,
                              NULL, &third) ==
                              VPS_CONNECTION_POOL_INTERRUPTED,
                          "interrupt_wait");
    wrong_key = config.key;
    wrong_key.identity = other_identity;
    passed &= expect_true(vps_connection_pool_acquire(
                              pool, &wrong_key, 1U, NULL, NULL, &third) ==
                              VPS_CONNECTION_POOL_KEY_MISMATCH,
                          "identity_mismatch");
    wrong_key = config.key;
    wrong_key.configuration_generation += 1U;
    passed &= expect_true(vps_connection_pool_acquire(
                              pool, &wrong_key, 1U, NULL, NULL, &third) ==
                              VPS_CONNECTION_POOL_KEY_MISMATCH,
                          "generation_mismatch");
    wrong_key = config.key;
    wrong_key.read_only = 0;
    passed &= expect_true(vps_connection_pool_acquire(
                              pool, &wrong_key, 1U, NULL, NULL, &third) ==
                              VPS_CONNECTION_POOL_KEY_MISMATCH,
                          "read_only_mismatch");
    passed &= expect_true(vps_connection_lease_release(
                              &first, VPS_CONNECTION_LEASE_CLEAN) ==
                              VPS_CONNECTION_POOL_OK,
                          "release_first");
    passed &= expect_true(vps_connection_lease_release(
                              &first, VPS_CONNECTION_LEASE_CLEAN) ==
                              VPS_CONNECTION_POOL_STALE_LEASE,
                          "double_release");
    passed &= expect_true(vps_connection_lease_release(
                              &second, VPS_CONNECTION_LEASE_DIRTY) ==
                              VPS_CONNECTION_POOL_OK,
                          "dirty_release");
    passed &= expect_true(vps_connection_pool_stats(pool, &stats) ==
                                  VPS_CONNECTION_POOL_OK &&
                              stats.total == 1U && stats.active == 0U &&
                              stats.idle == 1U,
                          "stats_after_dirty");
    passed &= expect_true(vps_connection_pool_destroy(&pool) ==
                                  VPS_CONNECTION_POOL_OK &&
                              backend.destroy_count == backend.create_count,
                          "destroy_exact");

    (void)memset(&backend, 0, sizeof(backend));
    config = make_config(allocator, &backend, identity, 0U, 4U);
    passed &= expect_true(vps_connection_pool_create(&config, &pool) ==
                              VPS_CONNECTION_POOL_OK,
                          "concurrency_create");
#if defined(_WIN32)
    {
        HANDLE handles[VPS_POOL_TEST_WORKERS];
        WorkerContext workers[VPS_POOL_TEST_WORKERS];
        uint32_t index;
        for (index = 0U; index < VPS_POOL_TEST_WORKERS; ++index) {
            workers[index].pool = pool;
            workers[index].key = config.key;
            workers[index].failures = 0;
            handles[index] = CreateThread(NULL, 0U, worker_main,
                                          &workers[index], 0U, NULL);
            passed &= expect_true(handles[index] != NULL, "thread_create");
        }
        passed &= expect_true(WaitForMultipleObjects(
                                  VPS_POOL_TEST_WORKERS, handles, TRUE,
                                  60000U) == WAIT_OBJECT_0,
                              "thread_wait");
        for (index = 0U; index < VPS_POOL_TEST_WORKERS; ++index) {
            passed &= expect_true(workers[index].failures == 0,
                                  "worker_failures");
            if (handles[index] != NULL) {
                (void)CloseHandle(handles[index]);
            }
        }
    }
#endif
    passed &= expect_true(vps_connection_pool_stats(pool, &stats) ==
                                  VPS_CONNECTION_POOL_OK &&
                              stats.active == 0U && stats.total <= 4U,
                          "concurrency_stats");
    passed &= expect_true(vps_connection_pool_destroy(&pool) ==
                                  VPS_CONNECTION_POOL_OK &&
                              backend.destroy_count == backend.create_count,
                          "concurrency_destroy");

#if defined(_WIN32)
    (void)memset(&backend, 0, sizeof(backend));
    config = make_config(allocator, &backend, identity, 0U, 1U);
    passed &= expect_true(vps_connection_pool_create(&config, &pool) ==
                              VPS_CONNECTION_POOL_OK,
                          "fifo_create");
    passed &= expect_true(vps_connection_pool_acquire(
                              pool, &config.key, 100U, NULL, NULL, &first) ==
                              VPS_CONNECTION_POOL_OK,
                          "fifo_initial_lease");
    {
        WaitingContext waiting[3];
        HANDLE handles[3];
        volatile long sequence = 0;
        uint32_t index;
        for (index = 0U; index < 3U; ++index) {
            (void)memset(&waiting[index], 0, sizeof(waiting[index]));
            waiting[index].pool = pool;
            waiting[index].key = config.key;
            waiting[index].sequence = &sequence;
            handles[index] = CreateThread(NULL, 0U, waiting_main,
                                          &waiting[index], 0U, NULL);
            passed &= expect_true(handles[index] != NULL, "fifo_thread");
            if (handles[index] != NULL) {
                ULONGLONG deadline = GetTickCount64() + 5000U;
                do {
                    (void)vps_connection_pool_stats(pool, &stats);
                    if (stats.waiting == index + 1U) {
                        break;
                    }
                    Sleep(1U);
                } while (GetTickCount64() < deadline);
                passed &= expect_true(stats.waiting == index + 1U,
                                      "fifo_registration");
            }
        }
        passed &= expect_true(vps_connection_lease_release(
                                  &first, VPS_CONNECTION_LEASE_CLEAN) ==
                                  VPS_CONNECTION_POOL_OK,
                              "fifo_release");
        passed &= expect_true(WaitForMultipleObjects(3U, handles, TRUE,
                                                     5000U) == WAIT_OBJECT_0,
                              "fifo_wait");
        for (index = 0U; index < 3U; ++index) {
            passed &= expect_true(waiting[index].result ==
                                          VPS_CONNECTION_POOL_OK &&
                                      waiting[index].acquired_order ==
                                          (long)index + 1L,
                                  "fifo_order");
            if (handles[index] != NULL) {
                (void)CloseHandle(handles[index]);
            }
        }
    }
    passed &= expect_true(vps_connection_pool_destroy(&pool) ==
                              VPS_CONNECTION_POOL_OK,
                          "fifo_destroy");

    (void)memset(&backend, 0, sizeof(backend));
    config = make_config(allocator, &backend, identity, 0U, 1U);
    passed &= expect_true(vps_connection_pool_create(&config, &pool) ==
                              VPS_CONNECTION_POOL_OK,
                          "close_waiter_create");
    passed &= expect_true(vps_connection_pool_acquire(
                              pool, &config.key, 100U, NULL, NULL, &first) ==
                              VPS_CONNECTION_POOL_OK,
                          "close_waiter_lease");
    {
        WaitingContext waiting;
        HANDLE handle;
        waiting.pool = pool;
        waiting.key = config.key;
        waiting.result = VPS_CONNECTION_POOL_OK;
        waiting.sequence = NULL;
        waiting.acquired_order = 0;
        handle = CreateThread(NULL, 0U, waiting_main, &waiting, 0U, NULL);
        passed &= expect_true(handle != NULL, "close_waiter_thread");
        if (handle != NULL) {
            ULONGLONG deadline = GetTickCount64() + 5000U;
            do {
                (void)vps_connection_pool_stats(pool, &stats);
                if (stats.waiting == 1U) {
                    break;
                }
                Sleep(1U);
            } while (GetTickCount64() < deadline);
            passed &= expect_true(stats.waiting == 1U,
                                  "close_waiter_registered");
            passed &= expect_true(vps_connection_pool_close(pool) ==
                                      VPS_CONNECTION_POOL_OK,
                                  "close_waiter_close");
            passed &= expect_true(WaitForSingleObject(handle, 5000U) ==
                                      WAIT_OBJECT_0,
                                  "close_waiter_woken");
            passed &= expect_true(waiting.result ==
                                      VPS_CONNECTION_POOL_CLOSED,
                                  "close_waiter_result");
            (void)CloseHandle(handle);
        }
    }
    passed &= expect_true(vps_connection_pool_destroy(&pool) ==
                              VPS_CONNECTION_POOL_BUSY,
                          "destroy_with_lease");
    passed &= expect_true(vps_connection_lease_release(
                              &first, VPS_CONNECTION_LEASE_CLEAN) ==
                              VPS_CONNECTION_POOL_OK,
                          "closed_lease_release");
    passed &= expect_true(vps_connection_pool_destroy(&pool) ==
                                  VPS_CONNECTION_POOL_OK &&
                              backend.destroy_count == backend.create_count,
                          "close_waiter_destroy");
#endif

    (void)memset(&backend, 0, sizeof(backend));
    backend.fail_create_at = 2U;
    config = make_config(allocator, &backend, identity, 2U, 2U);
    passed &= expect_true(vps_connection_pool_create(&config, &pool) ==
                                  VPS_CONNECTION_POOL_CREATE_FAILED &&
                              pool == NULL && backend.destroy_count == 1U,
                          "partial_init_cleanup");

    {
        size_t fail_at;
        for (fail_at = 1U; fail_at <= 4U; ++fail_at) {
            VpsFaultAllocator fault;
            VpsAllocator failing;
            (void)memset(&backend, 0, sizeof(backend));
            passed &= expect_true(vps_fault_allocator_init(
                                      &fault, &allocator, fail_at) ==
                                      VPS_MEMORY_OK,
                                  "fault_init");
            passed &= expect_true(vps_fault_allocator_make(&fault, &failing) ==
                                      VPS_MEMORY_OK,
                                  "fault_make");
            config = make_config(failing, &backend, identity, 0U, 2U);
            passed &= expect_true(vps_connection_pool_create(&config, &pool) ==
                                          VPS_CONNECTION_POOL_OUT_OF_MEMORY &&
                                      pool == NULL &&
                                      fault.active_allocations == 0U,
                                  "fault_cleanup");
        }
    }

    (void)printf("connection_pool workers=%u iterations=%u status=%s\n",
                 VPS_POOL_TEST_WORKERS, VPS_POOL_TEST_ITERATIONS,
                 passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
