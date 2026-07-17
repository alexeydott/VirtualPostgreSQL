#include "vps_module.h"
SQLITE_EXTENSION_INIT3

#include <string.h>

typedef struct VpsEmptyTable {
    sqlite3_vtab base;
} VpsEmptyTable;

typedef struct VpsEmptyCursor {
    sqlite3_vtab_cursor base;
    int is_eof;
} VpsEmptyCursor;

static int vps_module_connect(sqlite3 *database,
                              void *auxiliary,
                              int argument_count,
                              const char *const *arguments,
                              sqlite3_vtab **table_out,
                              char **error_out)
{
    VpsEmptyTable *table;
    int result;

    (void)auxiliary;
    (void)argument_count;
    (void)arguments;
    (void)error_out;
    if (database == NULL || table_out == NULL) {
        return SQLITE_MISUSE;
    }
    result = sqlite3_declare_vtab(database,
                                 "CREATE TABLE x(stage1_status TEXT)");
    if (result != SQLITE_OK) {
        return result;
    }
    result = sqlite3_vtab_config(database, SQLITE_VTAB_DIRECTONLY);
    if (result != SQLITE_OK) {
        return result;
    }
    table = (VpsEmptyTable *)sqlite3_malloc64(sizeof(*table));
    if (table == NULL) {
        return SQLITE_NOMEM;
    }
    (void)memset(table, 0, sizeof(*table));
    *table_out = &table->base;
    return SQLITE_OK;
}

static int vps_module_disconnect(sqlite3_vtab *table)
{
    sqlite3_free(table);
    return SQLITE_OK;
}

static int vps_module_best_index(sqlite3_vtab *table,
                                 sqlite3_index_info *index_info)
{
    (void)table;
    if (index_info == NULL) {
        return SQLITE_MISUSE;
    }
    index_info->estimatedCost = 1.0;
    index_info->estimatedRows = 0;
    return SQLITE_OK;
}

static int vps_module_open(sqlite3_vtab *table,
                           sqlite3_vtab_cursor **cursor_out)
{
    VpsEmptyCursor *cursor;

    (void)table;
    if (cursor_out == NULL) {
        return SQLITE_MISUSE;
    }
    cursor = (VpsEmptyCursor *)sqlite3_malloc64(sizeof(*cursor));
    if (cursor == NULL) {
        return SQLITE_NOMEM;
    }
    (void)memset(cursor, 0, sizeof(*cursor));
    cursor->is_eof = 1;
    *cursor_out = &cursor->base;
    return SQLITE_OK;
}

static int vps_module_close(sqlite3_vtab_cursor *cursor)
{
    sqlite3_free(cursor);
    return SQLITE_OK;
}

static int vps_module_filter(sqlite3_vtab_cursor *cursor,
                             int index_number,
                             const char *index_string,
                             int argument_count,
                             sqlite3_value **arguments)
{
    VpsEmptyCursor *empty_cursor = (VpsEmptyCursor *)cursor;

    (void)index_number;
    (void)index_string;
    (void)argument_count;
    (void)arguments;
    if (empty_cursor == NULL) {
        return SQLITE_MISUSE;
    }
    empty_cursor->is_eof = 1;
    return SQLITE_OK;
}

static int vps_module_next(sqlite3_vtab_cursor *cursor)
{
    VpsEmptyCursor *empty_cursor = (VpsEmptyCursor *)cursor;

    if (empty_cursor == NULL) {
        return SQLITE_MISUSE;
    }
    empty_cursor->is_eof = 1;
    return SQLITE_OK;
}

static int vps_module_eof(sqlite3_vtab_cursor *cursor)
{
    const VpsEmptyCursor *empty_cursor = (const VpsEmptyCursor *)cursor;
    return empty_cursor == NULL || empty_cursor->is_eof;
}

static int vps_module_column(sqlite3_vtab_cursor *cursor,
                             sqlite3_context *context,
                             int column)
{
    (void)cursor;
    (void)column;
    sqlite3_result_null(context);
    return SQLITE_OK;
}

static int vps_module_rowid(sqlite3_vtab_cursor *cursor,
                            sqlite3_int64 *rowid_out)
{
    (void)cursor;
    if (rowid_out == NULL) {
        return SQLITE_MISUSE;
    }
    *rowid_out = 0;
    return SQLITE_OK;
}

static int vps_module_integrity(sqlite3_vtab *table,
                                const char *schema,
                                const char *table_name,
                                int flags,
                                char **error_out)
{
    (void)table;
    (void)schema;
    (void)table_name;
    (void)flags;
    if (error_out != NULL) {
        *error_out = NULL;
    }
    return SQLITE_OK;
}

const sqlite3_module VPS_MODULE = {
    4,
    vps_module_connect,
    vps_module_connect,
    vps_module_best_index,
    vps_module_disconnect,
    vps_module_disconnect,
    vps_module_open,
    vps_module_close,
    vps_module_filter,
    vps_module_next,
    vps_module_eof,
    vps_module_column,
    vps_module_rowid,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL,
    NULL,
    vps_module_integrity
};
