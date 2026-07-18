#include "vps_query_cache.h"

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define CHECK(value)                                                        \
    do {                                                                    \
        if (!(value)) {                                                     \
            (void)fprintf(stderr, "[query-cache] failed: %s:%d\n",         \
                          __FILE__, __LINE__);                               \
            return 1;                                                       \
        }                                                                   \
    } while (0)

typedef struct BuildContext {
    VpsQueryCache *cache;
    size_t calls;
    int fail;
    uint32_t delay_ms;
} BuildContext;

static VpsQueryCacheStatus build_snapshot(void *opaque,
                                          VpsEmbeddedSqlite *database)
{
    BuildContext *context = (BuildContext *)opaque;
    VpsEmbeddedValueKind kind = VPS_EMBEDDED_VALUE_INTEGER;
    VpsEmbeddedSchema schema;
    VpsEmbeddedValue value;
    context->calls += 1U;
    vps_query_cache_record_remote_execution(context->cache);
#if defined(_WIN32)
    if (context->delay_ms != 0U) Sleep(context->delay_ms);
#endif
    if (context->fail) return VPS_QUERY_CACHE_BUILD_FAILED;
    (void)memset(&schema, 0, sizeof(schema));
    schema.column_kinds = &kind;
    schema.column_count = 1U;
    schema.source_fingerprint = UINT64_C(11);
    schema.layout_fingerprint = UINT64_C(12);
    if (vps_embedded_sqlite_create_schema(database, &schema) !=
        VPS_EMBEDDED_SQLITE_OK) return VPS_QUERY_CACHE_BUILD_FAILED;
    (void)memset(&value, 0, sizeof(value));
    value.kind = VPS_EMBEDDED_VALUE_INTEGER;
    value.integer = 42;
    return vps_embedded_sqlite_append_row(database, &value, 1U) ==
                   VPS_EMBEDDED_SQLITE_OK
               ? VPS_QUERY_CACHE_OK : VPS_QUERY_CACHE_BUILD_FAILED;
}

#if defined(_WIN32)
typedef struct AcquireThread {
    VpsQueryCache *cache;
    BuildContext *build;
    VpsQueryCacheLease lease;
    VpsQueryCacheStatus status;
} AcquireThread;

static DWORD WINAPI acquire_thread(void *opaque)
{
    AcquireThread *thread = (AcquireThread *)opaque;
    thread->status = vps_query_cache_acquire(
        thread->cache, 5000U, build_snapshot, thread->build, &thread->lease);
    return 0U;
}
#endif

int main(void)
{
    VpsAllocator allocator;
    VpsQueryCacheConfig config;
    VpsQueryCache *cache = NULL;
    VpsQueryCacheLease first;
    VpsQueryCacheLease second;
    BuildContext context;
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    (void)memset(&config, 0, sizeof(config));
    config.allocator = allocator;
    config.platform = vps_platform_current_operations();
    config.mode = VPS_EMBEDDED_SQLITE_MEMORY;
    config.source_fingerprint = UINT64_C(11);
    config.layout_fingerprint = UINT64_C(12);
    config.wait_slice_ms = VPS_QUERY_CACHE_DEFAULT_WAIT_SLICE_MS;
    CHECK(vps_query_cache_create(&config, &cache) == VPS_QUERY_CACHE_OK);
    (void)memset(&context, 0, sizeof(context));
    context.cache = cache;
    context.fail = 1;
    (void)memset(&first, 0, sizeof(first));
    CHECK(vps_query_cache_acquire(cache, 1000U, build_snapshot, &context,
                                  &first) == VPS_QUERY_CACHE_BUILD_FAILED);
    CHECK(vps_query_cache_state(cache) == VPS_QUERY_CACHE_EMPTY);
    CHECK(vps_query_cache_generation(cache) == 0U);
    context.fail = 0;
    CHECK(vps_query_cache_acquire(cache, 1000U, build_snapshot, &context,
                                  &first) == VPS_QUERY_CACHE_OK);
    CHECK(vps_query_cache_state(cache) == VPS_QUERY_CACHE_READY);
    CHECK(first.generation == 1U);
    CHECK(vps_embedded_sqlite_row_count(first.database) == 1U);
    (void)memset(&second, 0, sizeof(second));
    CHECK(vps_query_cache_acquire(cache, 1000U, build_snapshot, &context,
                                  &second) == VPS_QUERY_CACHE_OK);
    CHECK(second.database == first.database && second.generation == 1U);
    CHECK(context.calls == 2U);
    CHECK(vps_query_cache_build_attempts(cache) == 2U);
    CHECK(vps_query_cache_remote_executions(cache) == 2U);
    CHECK(vps_query_cache_destroy(&cache) == VPS_QUERY_CACHE_BUSY);
    CHECK(vps_query_cache_lease_release(&second) == VPS_QUERY_CACHE_OK);
    CHECK(vps_query_cache_lease_release(&first) == VPS_QUERY_CACHE_OK);
    CHECK(vps_query_cache_destroy(&cache) == VPS_QUERY_CACHE_OK);
    CHECK(cache == NULL);
#if defined(_WIN32)
    {
        AcquireThread threads[2];
        HANDLE handles[2] = {NULL, NULL};
        size_t index;
        CHECK(vps_query_cache_create(&config, &cache) == VPS_QUERY_CACHE_OK);
        (void)memset(&context, 0, sizeof(context));
        context.cache = cache;
        context.delay_ms = 100U;
        (void)memset(threads, 0, sizeof(threads));
        for (index = 0U; index < 2U; ++index) {
            threads[index].cache = cache;
            threads[index].build = &context;
            handles[index] = CreateThread(NULL, 0U, acquire_thread,
                                          &threads[index], 0U, NULL);
            if (handles[index] == NULL) return 1;
        }
        CHECK(WaitForMultipleObjects(2U, handles, TRUE, 5000U) ==
              WAIT_OBJECT_0);
        for (index = 0U; index < 2U; ++index) {
            (void)CloseHandle(handles[index]);
            CHECK(threads[index].status == VPS_QUERY_CACHE_OK);
        }
        CHECK(context.calls == 1U);
        CHECK(vps_query_cache_remote_executions(cache) == 1U);
        CHECK(threads[0].lease.database == threads[1].lease.database);
        CHECK(vps_query_cache_lease_release(&threads[0].lease) ==
              VPS_QUERY_CACHE_OK);
        CHECK(vps_query_cache_lease_release(&threads[1].lease) ==
              VPS_QUERY_CACHE_OK);
        CHECK(vps_query_cache_destroy(&cache) == VPS_QUERY_CACHE_OK);
    }
#endif
    (void)printf("[query-cache] attempts=2 generation=1 executions=2 "
                 "status=passed\n");
    return 0;
}
