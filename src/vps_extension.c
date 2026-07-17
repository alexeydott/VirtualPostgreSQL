#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include "vps_module.h"

#include <stdint.h>

#define VPS_MINIMUM_SQLITE_VERSION_NUMBER 3044000
#define VPS_EXTENSION_VERSION "0.1.0-stage1"

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

static void vps_not_linked_sql(sqlite3_context *context,
                               int argument_count,
                               sqlite3_value **arguments)
{
    (void)argument_count;
    (void)arguments;
    sqlite3_result_text(context, "not-linked-stage1", -1, SQLITE_STATIC);
}

static void vps_capabilities_sql(sqlite3_context *context,
                                 int argument_count,
                                 sqlite3_value **arguments)
{
    (void)argument_count;
    (void)arguments;
    sqlite3_result_text(context,
                        "stage1,module-v4,xIntegrity,directonly", -1,
                        SQLITE_STATIC);
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
                                     NULL, vps_not_linked_sql, NULL, NULL);
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_create_function(database,
                                     "virtualpostgresql_embedded_sqlite", 0,
                                     SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                                     vps_not_linked_sql, NULL, NULL);
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
    return sqlite3_create_module_v2(database, "VirtualPostgreSQL", &VPS_MODULE,
                                    NULL, NULL);
}
