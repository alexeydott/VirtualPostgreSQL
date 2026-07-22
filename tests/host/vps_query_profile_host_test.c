#include "sqlite3.h"
#include "virtualpostgresql/vps_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

typedef int (*VpsRegisterQueryProfileProviderFn)(
    sqlite3 *database,
    uint32_t source,
    uint64_t provider_id,
    const VpsQueryProfileProvider *provider);

typedef struct VpsProfileState {
    size_t resolves;
    size_t releases;
} VpsProfileState;

static int32_t vps_profile_resolve(void *context,
                                   const char *profile_name,
                                   uint32_t profile_name_length,
                                   VpsQueryProfileLease *lease)
{
    static const char expected_name[] = "live_smoke";
    static const char query[] =
        "SELECT 23::pg_catalog.int8 AS id, "
        "'query-profile-ok'::pg_catalog.text AS label";
    VpsProfileState *state = (VpsProfileState *)context;
    if (state == NULL || profile_name == NULL || lease == NULL ||
        lease->header.structure_size < sizeof(*lease) ||
        lease->header.api_version != VPS_API_VERSION)
        return VPS_QUERY_PROFILE_PROVIDER_ERROR;
    state->resolves += 1U;
    if (profile_name_length != sizeof(expected_name) - 1U ||
        memcmp(profile_name, expected_name, sizeof(expected_name) - 1U) != 0)
        return VPS_QUERY_PROFILE_PROVIDER_NOT_FOUND;
    lease->query = query;
    lease->query_length = (uint32_t)(sizeof(query) - 1U);
    lease->profile_revision = UINT64_C(5);
    lease->provider_lease = state;
    return VPS_QUERY_PROFILE_PROVIDER_OK;
}

static void vps_profile_release(void *context, VpsQueryProfileLease *lease)
{
    VpsProfileState *state = (VpsProfileState *)context;
    if (state != NULL && lease != NULL && lease->provider_lease == state)
        state->releases += 1U;
}

static char *vps_runtime_connstr(void)
{
#if defined(_WIN32)
    char *value = NULL;
    size_t length = 0U;
    if (_dupenv_s(&value, &length, "VPS_VTAB_TEST_CONNSTR") != 0)
        return NULL;
    return value;
#else
    const char *source = getenv("VPS_VTAB_TEST_CONNSTR");
    size_t length;
    char *value;
    if (source == NULL) return NULL;
    length = strlen(source) + 1U;
    value = (char *)malloc(length);
    if (value != NULL) (void)memcpy(value, source, length);
    return value;
#endif
}

int main(int argument_count, char **arguments)
{
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    char *error_message = NULL;
    char *sql = NULL;
    char *connstr = vps_runtime_connstr();
    VpsQueryProfileProvider provider;
    VpsProfileState state = {0};
    int result;
    int passed = 1;
#if defined(_WIN32)
    HMODULE module = NULL;
    VpsRegisterQueryProfileProviderFn register_provider = NULL;
#endif

    if (argument_count != 2) return 2;
    if (connstr == NULL || connstr[0] == '\0') {
        free(connstr);
        (void)puts("query_profile_host status=skipped reason=no_fixture");
        return 77;
    }
    (void)memset(&provider, 0, sizeof(provider));
    result = sqlite3_open(":memory:", &database);
    passed &= result == SQLITE_OK;
    if (passed)
        passed &= sqlite3_enable_load_extension(database, 1) == SQLITE_OK;
    if (passed)
        passed &= sqlite3_load_extension(
                      database, arguments[1],
                      "sqlite3_virtualpostgresql_init", &error_message) ==
                  SQLITE_OK;
    sqlite3_free(error_message);
    error_message = NULL;
#if defined(_WIN32)
    if (passed) {
        module = LoadLibraryA(arguments[1]);
        if (module != NULL)
            register_provider =
                (VpsRegisterQueryProfileProviderFn)(void *)GetProcAddress(
                    module,
                    "virtualpostgresql_register_query_profile_provider");
        passed &= module != NULL && register_provider != NULL;
    }
    if (passed) {
        provider.header.structure_size = (uint32_t)sizeof(provider);
        provider.header.api_version = VPS_API_VERSION;
        provider.header.present_fields =
            VPS_QUERY_PROFILE_PROVIDER_FIELDS_CURRENT;
        provider.resolve = vps_profile_resolve;
        provider.release = vps_profile_release;
        provider.provider_context = &state;
        passed &= register_provider(
                      database, VPS_QUERY_PROFILE_SOURCE_HOST, UINT64_C(101),
                      &provider) == SQLITE_OK;
    }
#else
    passed = 0;
#endif
    if (passed) {
        sql = sqlite3_mprintf(
            "CREATE VIRTUAL TABLE temp.vps_profile_live USING "
            "VirtualPostgreSQL(connstr=%Q,source=query,"
            "query_profile=live_smoke,mode=ro,key_columns=id)", connstr);
        passed &= sql != NULL;
    }
    if (passed)
        passed &= sqlite3_exec(database, sql, NULL, NULL, &error_message) ==
                  SQLITE_OK;
    sqlite3_free(sql);
    sqlite3_free(error_message);
    error_message = NULL;
    if (passed)
        passed &= sqlite3_prepare_v2(
                      database,
                      "SELECT id,label FROM temp.vps_profile_live", -1,
                      &statement, NULL) == SQLITE_OK;
    if (passed) {
        const unsigned char *label;
        passed &= sqlite3_step(statement) == SQLITE_ROW;
        label = passed ? sqlite3_column_text(statement, 1) : NULL;
        passed &= sqlite3_column_int64(statement, 0) == 23 &&
                  label != NULL &&
                  strcmp((const char *)label, "query-profile-ok") == 0 &&
                  sqlite3_step(statement) == SQLITE_DONE;
    }
    (void)sqlite3_finalize(statement);
    passed &= state.resolves != 0U && state.resolves == state.releases;
    if (database != NULL) passed &= sqlite3_close(database) == SQLITE_OK;
#if defined(_WIN32)
    if (module != NULL) (void)FreeLibrary(module);
#endif
    free(connstr);
    (void)printf(
        "query_profile_host resolves=%llu releases=%llu status=%s\n",
        (unsigned long long)state.resolves,
        (unsigned long long)state.releases,
        passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
