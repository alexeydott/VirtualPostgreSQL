#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include "vps_module.h"
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
#include "vps_embedded_sqlite.h"
#endif
#include "vps_libpq_client.h"
#include "virtualpostgresql/vps_api.h"

#include <stdint.h>

#define VPS_MINIMUM_SQLITE_VERSION_NUMBER 3044000
#define VPS_EXTENSION_VERSION "0.9.0"

int32_t VPS_CALL virtualpostgresql_cancel(sqlite3 *database)
{
    VpsModuleContext *context;
    if (database == NULL) return VPS_CANCEL_INVALID_DATABASE;
    context = (VpsModuleContext *)sqlite3_get_clientdata(
        database, VPS_MODULE_CLIENTDATA_KEY);
    if (context == NULL || context->closing ||
        !context->initialized_cancel_registry)
        return VPS_CANCEL_UNAVAILABLE;
    return vps_cancel_registry_request_all(&context->cancel_registry, NULL) ==
                   VPS_CANCEL_REGISTRY_OK
               ? VPS_CANCEL_OK
               : VPS_CANCEL_ERROR;
}

int VPS_CALL virtualpostgresql_register_credential_provider(
    sqlite3 *database, const VpsCredentialProvider *provider)
{
    VpsModuleContext *context;
    if (database == NULL || provider == NULL) return SQLITE_MISUSE;
    context = (VpsModuleContext *)sqlite3_get_clientdata(
        database, VPS_MODULE_CLIENTDATA_KEY);
    if (context == NULL || context->closing) return SQLITE_NOTFOUND;
#if defined(_WIN32)
    VpsCredentialRegistryResult result;
    if (!context->initialized_credential_registry) return SQLITE_NOTFOUND;
    result = vps_credential_registry_register(
        &context->credential_registry, UINT64_C(2), provider);
    if (result == VPS_CREDENTIAL_REGISTRY_OK) return SQLITE_OK;
    if (result == VPS_CREDENTIAL_REGISTRY_REPLACEMENT_FORBIDDEN ||
        result == VPS_CREDENTIAL_REGISTRY_BUSY)
        return SQLITE_BUSY;
    if (result == VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT ||
        result == VPS_CREDENTIAL_REGISTRY_ABI_INCOMPATIBLE)
        return SQLITE_MISUSE;
    return SQLITE_ERROR;
#else
    return SQLITE_NOTFOUND;
#endif
}

#if defined(_WIN32)
int VPS_CALL virtualpostgresql_wincred_provider(
    VpsCredentialProvider *provider)
{
    if (provider == NULL) return SQLITE_MISUSE;
    return vps_wincred_provider_public_make(provider) ==
                   VPS_CREDENTIAL_REGISTRY_OK
               ? SQLITE_OK
               : SQLITE_ERROR;
}
#endif

static void vps_version_sql(sqlite3_context *context,
                            int argument_count,
                            sqlite3_value **arguments)
{
    (void)argument_count;
    (void)arguments;
    sqlite3_result_text(context, VPS_EXTENSION_VERSION, -1, SQLITE_STATIC);
}

static void vps_build_arch_sql(sqlite3_context *context,
                               int argument_count,
                               sqlite3_value **arguments)
{
    (void)argument_count;
    (void)arguments;
    sqlite3_result_text(context, sizeof(void *) == 4 ? "x86" : "x64", -1,
                        SQLITE_STATIC);
}

static void vps_libpq_version_sql(sqlite3_context *context,
                                  int argument_count,
                                  sqlite3_value **arguments)
{
    (void)argument_count;
    (void)arguments;
    sqlite3_result_int(context, vps_libpq_client_library_version());
}

static void vps_embedded_sqlite_sql(sqlite3_context *context,
                                    int argument_count,
                                    sqlite3_value **arguments)
{
    (void)argument_count;
    (void)arguments;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    sqlite3_result_text(context, vps_embedded_sqlite_version(), -1,
                        SQLITE_TRANSIENT);
#else
    sqlite3_result_text(context, "disabled", -1, SQLITE_STATIC);
#endif
}

static void vps_capabilities_sql(sqlite3_context *context,
                                 int argument_count,
                                 sqlite3_value **arguments)
{
    (void)argument_count;
    (void)arguments;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    sqlite3_result_text(context,
                        "read-write,module-v4,xIntegrity,directonly,async-libpq,single-row,secure-cancel,host-cancel,planner,predicate-pushdown,projection-pushdown,order-pushdown,limit-pushdown,query-materialization-memory,query-materialization-temp,keyed-dml,transactions,savepoints,metadata-functions,metadata-cache", -1,
                        SQLITE_STATIC);
#else
    sqlite3_result_text(context,
                        "read-write,module-v4,xIntegrity,directonly,async-libpq,single-row,secure-cancel,host-cancel,planner,predicate-pushdown,projection-pushdown,order-pushdown,limit-pushdown,keyed-dml,transactions,savepoints,metadata-functions,metadata-cache", -1,
                        SQLITE_STATIC);
#endif
}

int vps_extension_check_host_version(int version_number)
{
    return version_number >= VPS_MINIMUM_SQLITE_VERSION_NUMBER ? SQLITE_OK
                                                                : SQLITE_ERROR;
}

#if defined(_WIN32)
__declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
int sqlite3_virtualpostgresql_init(sqlite3 *database,
                                   char **error_message,
                                   const sqlite3_api_routines *api)
{
    int result;
    VpsModuleContext *module_context;

    SQLITE_EXTENSION_INIT2(api);
    if (database == NULL || api == NULL) {
        return SQLITE_MISUSE;
    }
    if (vps_extension_check_host_version(sqlite3_libversion_number()) !=
        SQLITE_OK) {
        if (error_message != NULL) {
            *error_message = sqlite3_mprintf(
                "VirtualPostgreSQL requires SQLite 3.44.0 or newer");
        }
        return SQLITE_ERROR;
    }
    result = sqlite3_create_function(database, "virtualpostgresql_version", 0,
                                     SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                                     vps_version_sql, NULL, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_create_function(database, "virtualpostgresql_build_arch",
                                     0, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                     NULL, vps_build_arch_sql, NULL, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_create_function(database, "virtualpostgresql_libpq_version",
                                     0, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                     NULL, vps_libpq_version_sql, NULL, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_create_function(database,
                                     "virtualpostgresql_embedded_sqlite", 0,
                                     SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                                     vps_embedded_sqlite_sql, NULL, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_create_function(database,
                                     "virtualpostgresql_capabilities", 0,
                                     SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                                     vps_capabilities_sql, NULL, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    module_context = vps_module_context_create(database);
    if (module_context == NULL) {
        return SQLITE_NOMEM;
    }
    result = sqlite3_create_module_v2(database, "VirtualPostgreSQL",
                                      &VPS_MODULE, module_context,
                                      vps_module_context_destroy);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_create_module_v2(database,
                                      "virtualpostgresql_relations",
                                      &VPS_METADATA_MODULE, module_context,
                                      NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_create_module_v2(database,
                                      "virtualpostgresql_table_info",
                                      &VPS_METADATA_MODULE, module_context,
                                      NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_create_module_v2(database,
                                      "virtualpostgresql_index_list",
                                      &VPS_METADATA_MODULE, module_context,
                                      NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_create_module_v2(database,
                                      "virtualpostgresql_index_info",
                                      &VPS_METADATA_MODULE, module_context,
                                      NULL);
    if (result != SQLITE_OK) return result;
    result = sqlite3_create_module_v2(database,
                                      "virtualpostgresql_type_info",
                                      &VPS_METADATA_MODULE, module_context,
                                      NULL);
    if (result != SQLITE_OK) return result;
    result = sqlite3_create_module_v2(database,
                                      "virtualpostgresql_extensions",
                                      &VPS_METADATA_MODULE, module_context,
                                      NULL);
    if (result != SQLITE_OK) return result;
    return sqlite3_set_clientdata(database, VPS_MODULE_CLIENTDATA_KEY,
                                  module_context, NULL);
}
