#include "vps_query_profile.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

typedef struct FakeProvider {
    const char *query;
    uint64_t revision;
    size_t resolves;
    size_t releases;
    int32_t result;
} FakeProvider;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    (void)fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #condition); \
    ++failures; } } while (0)

static int32_t fake_resolve(void *context,
                            const char *name,
                            uint32_t name_length,
                            VpsQueryProfileLease *lease)
{
    FakeProvider *fake = (FakeProvider *)context;
    (void)name;
    (void)name_length;
    ++fake->resolves;
    if (fake->result != VPS_QUERY_PROFILE_PROVIDER_OK) return fake->result;
    lease->query = fake->query;
    lease->query_length = (uint32_t)strlen(fake->query);
    lease->profile_revision = fake->revision;
    lease->provider_lease = fake;
    return VPS_QUERY_PROFILE_PROVIDER_OK;
}

static void fake_release(void *context, VpsQueryProfileLease *lease)
{
    FakeProvider *fake = (FakeProvider *)context;
    CHECK(lease->provider_lease == fake);
    ++fake->releases;
    (void)memset(lease, 0, sizeof(*lease));
}

static void test_fault_cleanup(void)
{
    VpsAllocator backing;
    VpsAllocator allocator;
    VpsFaultAllocator fault;
    VpsQueryProfileRegistry registry;
    VpsResolvedQueryProfile resolved;
    VpsQueryProfileProvider provider;
    FakeProvider fake = {"SELECT 1", 3U, 0U, 0U,
                         VPS_QUERY_PROFILE_PROVIDER_OK};

    CHECK(vps_allocator_system(&backing) == VPS_MEMORY_OK);
    CHECK(vps_fault_allocator_init(&fault, &backing, 1U) == VPS_MEMORY_OK);
    CHECK(vps_fault_allocator_make(&fault, &allocator) == VPS_MEMORY_OK);
    CHECK(vps_query_profile_registry_init(
              &registry, vps_platform_current_operations(), NULL) ==
          VPS_QUERY_PROFILE_OK);
    (void)memset(&provider, 0, sizeof(provider));
    provider.header.structure_size = (uint32_t)sizeof(provider);
    provider.header.api_version = VPS_API_VERSION;
    provider.header.present_fields = VPS_QUERY_PROFILE_PROVIDER_FIELDS_CURRENT;
    provider.resolve = fake_resolve;
    provider.release = fake_release;
    provider.provider_context = &fake;
    provider.header.api_version = VPS_API_VERSION_ENCODE(2, 0, 0);
    CHECK(vps_query_profile_registry_register(
              &registry, VPS_QUERY_PROFILE_HOST, 91U, &provider) ==
          VPS_QUERY_PROFILE_ABI_INCOMPATIBLE);
    provider.header.api_version = VPS_API_VERSION;
    CHECK(vps_query_profile_registry_register(
              &registry, VPS_QUERY_PROFILE_HOST, 1U, &provider) ==
          VPS_QUERY_PROFILE_OK);
    CHECK(vps_resolved_query_profile_init(&resolved, &allocator) ==
          VPS_QUERY_PROFILE_OK);
    CHECK(vps_query_profile_registry_resolve(
              &registry, VPS_QUERY_PROFILE_HOST, "fault", 5U, &resolved) ==
          VPS_QUERY_PROFILE_OUT_OF_MEMORY);
    CHECK(fake.releases == 1U && fault.active_allocations == 0U);
    vps_resolved_query_profile_cleanup(&resolved);
    CHECK(vps_query_profile_registry_cleanup(&registry) ==
          VPS_QUERY_PROFILE_OK);
}

#if defined(_WIN32)
enum { VPS_QUERY_PROFILE_THREAD_COUNT = 8 };

typedef struct BlockingProvider {
    HANDLE entered;
    HANDLE proceed;
    volatile LONG active;
    volatile LONG releases;
} BlockingProvider;

typedef struct ResolveThread {
    VpsQueryProfileRegistry *registry;
    VpsQueryProfileResult result;
} ResolveThread;

static int32_t blocking_resolve(
    void *context, const char *name, uint32_t name_length,
    VpsQueryProfileLease *lease)
{
    static const char query[] = "SELECT 1 AS id";
    BlockingProvider *blocking = (BlockingProvider *)context;
    (void)name;
    (void)name_length;
    if (InterlockedIncrement(&blocking->active) ==
        VPS_QUERY_PROFILE_THREAD_COUNT) {
        (void)SetEvent(blocking->entered);
    }
    if (WaitForSingleObject(blocking->proceed, 5000U) != WAIT_OBJECT_0) {
        return VPS_QUERY_PROFILE_PROVIDER_UNAVAILABLE;
    }
    lease->query = query;
    lease->query_length = sizeof(query) - 1U;
    lease->profile_revision = 9U;
    lease->provider_lease = blocking;
    return VPS_QUERY_PROFILE_PROVIDER_OK;
}

static void blocking_release(void *context, VpsQueryProfileLease *lease)
{
    BlockingProvider *blocking = (BlockingProvider *)context;
    CHECK(lease->provider_lease == blocking);
    (void)InterlockedIncrement(&blocking->releases);
}

static DWORD WINAPI resolve_thread_main(void *context)
{
    ResolveThread *thread = (ResolveThread *)context;
    VpsAllocator allocator;
    VpsResolvedQueryProfile resolved;
    if (vps_allocator_system(&allocator) != VPS_MEMORY_OK ||
        vps_resolved_query_profile_init(&resolved, &allocator) !=
            VPS_QUERY_PROFILE_OK) {
        thread->result = VPS_QUERY_PROFILE_INVALID_ARGUMENT;
        return 1U;
    }
    thread->result = vps_query_profile_registry_resolve(
        thread->registry, VPS_QUERY_PROFILE_HOST, "concurrent", 10U,
        &resolved);
    vps_resolved_query_profile_cleanup(&resolved);
    return 0U;
}

static void test_concurrent_resolve(void)
{
    VpsQueryProfileRegistry registry;
    VpsQueryProfileProvider provider;
    BlockingProvider blocking = {0};
    ResolveThread threads[VPS_QUERY_PROFILE_THREAD_COUNT] = {0};
    HANDLE handles[VPS_QUERY_PROFILE_THREAD_COUNT] = {0};
    size_t index;

    blocking.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    blocking.proceed = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(blocking.entered != NULL && blocking.proceed != NULL);
    CHECK(vps_query_profile_registry_init(
              &registry, vps_platform_current_operations(), NULL) ==
          VPS_QUERY_PROFILE_OK);
    (void)memset(&provider, 0, sizeof(provider));
    provider.header.structure_size = (uint32_t)sizeof(provider);
    provider.header.api_version = VPS_API_VERSION;
    provider.header.present_fields = VPS_QUERY_PROFILE_PROVIDER_FIELDS_CURRENT;
    provider.resolve = blocking_resolve;
    provider.release = blocking_release;
    provider.provider_context = &blocking;
    CHECK(vps_query_profile_registry_register(
              &registry, VPS_QUERY_PROFILE_HOST, 2U, &provider) ==
          VPS_QUERY_PROFILE_OK);
    for (index = 0U; index < VPS_QUERY_PROFILE_THREAD_COUNT; ++index) {
        threads[index].registry = &registry;
        handles[index] = CreateThread(NULL, 0U, resolve_thread_main,
                                      &threads[index], 0U, NULL);
        CHECK(handles[index] != NULL);
    }
    CHECK(WaitForSingleObject(blocking.entered, 5000U) == WAIT_OBJECT_0);
    CHECK(vps_query_profile_registry_cleanup(&registry) ==
          VPS_QUERY_PROFILE_BUSY);
    (void)SetEvent(blocking.proceed);
    CHECK(WaitForMultipleObjects(VPS_QUERY_PROFILE_THREAD_COUNT, handles,
                                 TRUE, 5000U) == WAIT_OBJECT_0);
    for (index = 0U; index < VPS_QUERY_PROFILE_THREAD_COUNT; ++index) {
        CHECK(threads[index].result == VPS_QUERY_PROFILE_OK);
        if (handles[index] != NULL) (void)CloseHandle(handles[index]);
    }
    CHECK(blocking.releases == VPS_QUERY_PROFILE_THREAD_COUNT);
    CHECK(vps_query_profile_registry_cleanup(&registry) ==
          VPS_QUERY_PROFILE_OK);
    if (blocking.entered != NULL) (void)CloseHandle(blocking.entered);
    if (blocking.proceed != NULL) (void)CloseHandle(blocking.proceed);
}
#endif

int main(void)
{
    VpsAllocator allocator;
    VpsQueryProfileRegistry registry;
    VpsResolvedQueryProfile resolved;
    VpsQueryProfileProvider provider;
    FakeProvider fake = {"SELECT 1 AS id", 7U, 0U, 0U,
                         VPS_QUERY_PROFILE_PROVIDER_OK};

    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    CHECK(vps_query_profile_registry_init(
              &registry, vps_platform_current_operations(), NULL) ==
          VPS_QUERY_PROFILE_OK);
    (void)memset(&provider, 0, sizeof(provider));
    provider.header.structure_size = (uint32_t)sizeof(provider);
    provider.header.api_version = VPS_API_VERSION;
    provider.header.present_fields = VPS_QUERY_PROFILE_PROVIDER_FIELDS_CURRENT;
    provider.resolve = fake_resolve;
    provider.release = fake_release;
    provider.provider_context = &fake;
    CHECK(vps_query_profile_registry_register(
              &registry, VPS_QUERY_PROFILE_HOST, 91U, &provider) ==
          VPS_QUERY_PROFILE_OK);
    CHECK(vps_resolved_query_profile_init(&resolved, &allocator) ==
          VPS_QUERY_PROFILE_OK);
    CHECK(vps_query_profile_registry_resolve(
              &registry, VPS_QUERY_PROFILE_HOST, "active", 6U, &resolved) ==
          VPS_QUERY_PROFILE_OK);
    CHECK(fake.resolves == 1U && fake.releases == 1U);
    CHECK(resolved.profile_revision == 7U && resolved.provider_id == 91U);
    CHECK(resolved.query.size == strlen(fake.query) + 1U);
    CHECK(resolved.analysis.result == VPS_QUERY_SOURCE_OK);
    CHECK(vps_query_profile_registry_register(
              &registry, VPS_QUERY_PROFILE_HOST, 92U, &provider) ==
          VPS_QUERY_PROFILE_REPLACEMENT_FORBIDDEN);
    vps_resolved_query_profile_cleanup(&resolved);

    CHECK(vps_resolved_query_profile_init(&resolved, &allocator) ==
          VPS_QUERY_PROFILE_OK);
    fake.query = "DELETE FROM guarded";
    fake.revision = 8U;
    CHECK(vps_query_profile_registry_resolve(
              &registry, VPS_QUERY_PROFILE_HOST, "invalid", 7U, &resolved) ==
          VPS_QUERY_PROFILE_INVALID_QUERY);
    CHECK(fake.resolves == 2U && fake.releases == 2U);
    vps_resolved_query_profile_cleanup(&resolved);

    CHECK(vps_resolved_query_profile_init(&resolved, &allocator) ==
          VPS_QUERY_PROFILE_OK);
    CHECK(vps_query_profile_registry_resolve(
              &registry, VPS_QUERY_PROFILE_ENVIRONMENT, "missing", 7U,
              &resolved) == VPS_QUERY_PROFILE_NOT_REGISTERED);
    vps_resolved_query_profile_cleanup(&resolved);
    CHECK(vps_query_profile_registry_cleanup(&registry) ==
          VPS_QUERY_PROFILE_OK);

    test_fault_cleanup();
#if defined(_WIN32)
    test_concurrent_resolve();
#endif

    if (failures != 0) return 1;
    (void)puts("vps_query_profile_test: passed");
    return 0;
}
