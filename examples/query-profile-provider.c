#include <virtualpostgresql/vps_api.h>

#include <string.h>

#define REPORTING_PROFILE_NAME "recent_orders"
#define REPORTING_PROFILE_QUERY \
    "SELECT order_id, customer_id, created_at " \
    "FROM reporting.orders WHERE created_at >= CURRENT_DATE - 7"

static int32_t VPS_CALL resolve_query_profile(
    void *provider_context,
    const char *profile_name,
    uint32_t profile_name_length,
    VpsQueryProfileLease *lease)
{
    static const char query[] = REPORTING_PROFILE_QUERY;
    (void)provider_context;

    if (profile_name_length != sizeof(REPORTING_PROFILE_NAME) - 1U ||
        memcmp(profile_name, REPORTING_PROFILE_NAME,
               sizeof(REPORTING_PROFILE_NAME) - 1U) != 0) {
        return VPS_QUERY_PROFILE_PROVIDER_NOT_FOUND;
    }

    lease->header.structure_size = (uint32_t)sizeof(*lease);
    lease->header.api_version = VPS_API_VERSION;
    lease->header.present_fields = VPS_QUERY_PROFILE_LEASE_FIELDS_CURRENT;
    lease->query = query;
    lease->query_length = (uint32_t)(sizeof(query) - 1U);
    lease->profile_revision = UINT64_C(2026072201);
    lease->provider_lease = NULL;
    return VPS_QUERY_PROFILE_PROVIDER_OK;
}

static void VPS_CALL release_query_profile(
    void *provider_context,
    VpsQueryProfileLease *lease)
{
    (void)provider_context;
    (void)lease;
    /* Release provider-owned query state here; never log the query text. */
}

int register_query_profile_provider(sqlite3 *database)
{
    VpsQueryProfileProvider provider = {0};

    provider.header.structure_size = (uint32_t)sizeof(provider);
    provider.header.api_version = VPS_API_VERSION;
    provider.header.present_fields =
        VPS_QUERY_PROFILE_PROVIDER_FIELDS_CURRENT;
    provider.resolve = resolve_query_profile;
    provider.release = release_query_profile;
    provider.provider_context = NULL;

    return virtualpostgresql_register_query_profile_provider(
        database, VPS_QUERY_PROFILE_SOURCE_HOST, UINT64_C(1), &provider);
}
