#include "vps_embedded_sqlite.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

/* Generic sqlite3_* APIs have internal linkage in this translation unit. */
#define SQLITE_API static
#include "sqlite3.c"

struct VpsEmbeddedSqlite {
    VpsAllocator allocator;
    VpsLogger *logger;
    sqlite3 *database;
    char *temp_path;
    size_t temp_path_size;
    VpsEmbeddedSqliteMode mode;
    sqlite3_stmt *insert_statement;
    size_t column_count;
    size_t index_count;
    uint64_t source_fingerprint;
    uint64_t layout_fingerprint;
    uint64_t row_count;
    uint64_t byte_count;
    int transaction_active;
    int sealed;
};

struct VpsEmbeddedSqliteScan {
    VpsEmbeddedSqlite *owner;
    sqlite3_stmt *statement;
    int has_row;
    int uses_index;
};

static int vps_embedded_sql_append(VpsBuffer *sql,
                                   const char *value,
                                   size_t length)
{
    return vps_buffer_append(sql, value, length) == VPS_MEMORY_OK;
}

static int vps_embedded_sql_number(VpsBuffer *sql,
                                   const char *prefix,
                                   size_t value)
{
    char buffer[48];
    int length = snprintf(buffer, sizeof(buffer), "%s%llu", prefix,
                          (unsigned long long)value);
    return length > 0 && (size_t)length < sizeof(buffer) &&
           vps_embedded_sql_append(sql, buffer, (size_t)length);
}

static const char *vps_embedded_affinity(VpsEmbeddedValueKind kind)
{
    switch (kind) {
        case VPS_EMBEDDED_VALUE_INTEGER: return "INTEGER";
        case VPS_EMBEDDED_VALUE_REAL: return "REAL";
        case VPS_EMBEDDED_VALUE_BLOB: return "BLOB";
        case VPS_EMBEDDED_VALUE_NULL: case VPS_EMBEDDED_VALUE_TEXT:
        default: return "TEXT";
    }
}

static int vps_embedded_bind(sqlite3_stmt *statement,
                             int parameter,
                             const VpsEmbeddedValue *value)
{
    int length;
    if (statement == NULL || value == NULL || value->length > INT_MAX)
        return SQLITE_MISUSE;
    length = (int)value->length;
    switch (value->kind) {
        case VPS_EMBEDDED_VALUE_NULL:
            return sqlite3_bind_null(statement, parameter);
        case VPS_EMBEDDED_VALUE_INTEGER:
            return sqlite3_bind_int64(statement, parameter,
                                      (sqlite3_int64)value->integer);
        case VPS_EMBEDDED_VALUE_REAL:
            return sqlite3_bind_double(statement, parameter, value->real);
        case VPS_EMBEDDED_VALUE_TEXT:
            if (value->bytes == NULL && value->length != 0U)
                return SQLITE_MISUSE;
            return sqlite3_bind_text(statement, parameter,
                                     (const char *)value->bytes, length,
                                     SQLITE_TRANSIENT);
        case VPS_EMBEDDED_VALUE_BLOB:
            if (value->bytes == NULL && value->length != 0U)
                return SQLITE_MISUSE;
            return sqlite3_bind_blob(statement, parameter, value->bytes,
                                     length, SQLITE_TRANSIENT);
        default: return SQLITE_MISUSE;
    }
}

static uint64_t vps_embedded_hash_bytes(uint64_t hash,
                                        const char *value,
                                        size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        hash ^= (unsigned char)value[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void vps_embedded_log(VpsLogger *logger,
                             VpsLogLevel level,
                             const char *phase,
                             VpsEmbeddedSqliteMode mode,
                             VpsEmbeddedSqliteStatus status)
{
    VpsLogEvent event;
    const char *mode_name = mode == VPS_EMBEDDED_SQLITE_TEMP ? "temp" :
                                                                  "memory";
    if (logger == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK) return;
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                   "embedded-sqlite", 15U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                   strlen(phase));
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_SNAPSHOT_MODE,
                                   mode_name, strlen(mode_name));
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS,
                                   vps_embedded_sqlite_status_name(status),
                                   strlen(vps_embedded_sqlite_status_name(status)));
    vps_logger_emit(logger, &event);
}

static VpsEmbeddedSqliteStatus vps_embedded_status_from_sqlite(int status)
{
    switch (status & 0xff) {
        case SQLITE_OK: return VPS_EMBEDDED_SQLITE_OK;
        case SQLITE_NOMEM: return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
        case SQLITE_BUSY: case SQLITE_LOCKED: return VPS_EMBEDDED_SQLITE_BUSY;
        case SQLITE_FULL: case SQLITE_TOOBIG:
            return VPS_EMBEDDED_SQLITE_LIMIT_EXCEEDED;
        case SQLITE_CANTOPEN: case SQLITE_IOERR:
            return VPS_EMBEDDED_SQLITE_IO_ERROR;
        default: return VPS_EMBEDDED_SQLITE_ERROR;
    }
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_open(
    const VpsEmbeddedSqliteOpenOptions *options,
    VpsEmbeddedSqlite **database)
{
    VpsEmbeddedSqlite *candidate = NULL;
    const char *filename;
    size_t allocation_size;
    int sqlite_status;
    VpsEmbeddedSqliteStatus status;
    if (options == NULL || database == NULL || *database != NULL ||
        !vps_allocator_is_valid(&options->allocator) ||
        (options->mode != VPS_EMBEDDED_SQLITE_MEMORY &&
         options->mode != VPS_EMBEDDED_SQLITE_TEMP) ||
        (options->mode == VPS_EMBEDDED_SQLITE_TEMP &&
         (options->temp_path == NULL || options->temp_path_length == 0U)))
        return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    if (vps_memory_allocate(&options->allocator, sizeof(*candidate),
                            (void **)&candidate) != VPS_MEMORY_OK)
        return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
    (void)memset(candidate, 0, sizeof(*candidate));
    candidate->allocator = options->allocator;
    candidate->logger = options->logger;
    candidate->mode = options->mode;
    if (options->mode == VPS_EMBEDDED_SQLITE_TEMP) {
        if (vps_size_add(options->temp_path_length, 1U, &allocation_size) !=
                VPS_MEMORY_OK ||
            vps_memory_allocate(&candidate->allocator, allocation_size,
                                (void **)&candidate->temp_path) != VPS_MEMORY_OK) {
            vps_memory_release(&candidate->allocator, (void **)&candidate,
                               sizeof(*candidate));
            return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
        }
        (void)memcpy(candidate->temp_path, options->temp_path,
                     options->temp_path_length);
        candidate->temp_path[options->temp_path_length] = '\0';
        candidate->temp_path_size = allocation_size;
        filename = candidate->temp_path;
    } else {
        filename = ":memory:";
    }
    sqlite_status = sqlite3_open_v2(
        filename, &candidate->database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_PRIVATECACHE,
        NULL);
    status = vps_embedded_status_from_sqlite(sqlite_status);
    if (status == VPS_EMBEDDED_SQLITE_OK) {
        sqlite_status = sqlite3_extended_result_codes(candidate->database, 1);
        if (sqlite_status == SQLITE_OK)
            sqlite_status = sqlite3_db_config(candidate->database,
                                               SQLITE_DBCONFIG_DEFENSIVE, 1,
                                               NULL);
        if (sqlite_status == SQLITE_OK)
            sqlite_status = sqlite3_exec(
                candidate->database,
                "PRAGMA trusted_schema=OFF;PRAGMA foreign_keys=ON;"
                "PRAGMA temp_store=MEMORY;PRAGMA journal_mode=MEMORY;",
                NULL, NULL, NULL);
        status = vps_embedded_status_from_sqlite(sqlite_status);
    }
    if (status != VPS_EMBEDDED_SQLITE_OK) {
        if (candidate->database != NULL)
            (void)sqlite3_close_v2(candidate->database);
        if (candidate->temp_path != NULL) {
            (void)remove(candidate->temp_path);
            vps_memory_release(&candidate->allocator,
                               (void **)&candidate->temp_path,
                               candidate->temp_path_size);
        }
        vps_embedded_log(candidate->logger, VPS_LOG_LEVEL_ERROR, "open",
                         candidate->mode, status);
        vps_memory_release(&candidate->allocator, (void **)&candidate,
                           sizeof(*candidate));
        return status;
    }
    vps_embedded_log(candidate->logger, VPS_LOG_LEVEL_INFO, "open",
                     candidate->mode, VPS_EMBEDDED_SQLITE_OK);
    *database = candidate;
    return VPS_EMBEDDED_SQLITE_OK;
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_close(
    VpsEmbeddedSqlite **database)
{
    VpsEmbeddedSqlite *owned;
    VpsEmbeddedSqliteStatus status = VPS_EMBEDDED_SQLITE_OK;
    if (database == NULL) return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    owned = *database;
    if (owned == NULL) return VPS_EMBEDDED_SQLITE_OK;
    *database = NULL;
    if (owned->insert_statement != NULL) {
        (void)sqlite3_finalize(owned->insert_statement);
        owned->insert_statement = NULL;
    }
    if (owned->transaction_active && owned->database != NULL)
        (void)sqlite3_exec(owned->database, "ROLLBACK", NULL, NULL, NULL);
    if (owned->database != NULL) {
        status = vps_embedded_status_from_sqlite(
            sqlite3_close_v2(owned->database));
        owned->database = NULL;
    }
    if (owned->temp_path != NULL) {
        if (remove(owned->temp_path) != 0 && status == VPS_EMBEDDED_SQLITE_OK)
            status = VPS_EMBEDDED_SQLITE_IO_ERROR;
        vps_memory_release(&owned->allocator, (void **)&owned->temp_path,
                           owned->temp_path_size);
    }
    vps_embedded_log(owned->logger,
                     status == VPS_EMBEDDED_SQLITE_OK ? VPS_LOG_LEVEL_INFO
                                                       : VPS_LOG_LEVEL_WARN,
                     "close", owned->mode, status);
    vps_memory_release(&owned->allocator, (void **)&owned, sizeof(*owned));
    return status;
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_create_schema(
    VpsEmbeddedSqlite *database,
    const VpsEmbeddedSchema *schema)
{
    VpsBuffer sql;
    size_t index;
    int sqlite_status;
    if (database == NULL || database->database == NULL || schema == NULL ||
        schema->column_kinds == NULL || schema->column_count == 0U ||
        schema->column_count > VPS_EMBEDDED_MAX_COLUMNS ||
        schema->index_count > VPS_EMBEDDED_MAX_INDEXES ||
        (schema->index_count != 0U && schema->indexes == NULL) ||
        database->transaction_active || database->sealed)
        return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    if (vps_buffer_init(&sql, &database->allocator, 65536U) != VPS_MEMORY_OK)
        return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
    if (!vps_embedded_sql_append(&sql, "CREATE TABLE vps_snapshot(", 26U))
        goto memory_error;
    for (index = 0U; index < schema->column_count; ++index) {
        const char *affinity = vps_embedded_affinity(schema->column_kinds[index]);
        if ((index != 0U && !vps_embedded_sql_append(&sql, ",", 1U)) ||
            !vps_embedded_sql_number(&sql, "c", index) ||
            !vps_embedded_sql_append(&sql, " ", 1U) ||
            !vps_embedded_sql_append(&sql, affinity, strlen(affinity)))
            goto memory_error;
    }
    if (!vps_embedded_sql_append(&sql, ")\0", 2U)) goto memory_error;
    sqlite_status = sqlite3_exec(database->database, "BEGIN IMMEDIATE", NULL,
                                 NULL, NULL);
    if (sqlite_status != SQLITE_OK) goto sqlite_error;
    database->transaction_active = 1;
    sqlite_status = sqlite3_exec(database->database, (const char *)sql.data,
                                 NULL, NULL, NULL);
    if (sqlite_status != SQLITE_OK) goto sqlite_error;
    sql.size = 0U;
    if (!vps_embedded_sql_append(&sql, "INSERT INTO vps_snapshot VALUES(", 32U))
        goto memory_error;
    for (index = 0U; index < schema->column_count; ++index) {
        if ((index != 0U && !vps_embedded_sql_append(&sql, ",", 1U)) ||
            !vps_embedded_sql_append(&sql, "?", 1U)) goto memory_error;
    }
    if (!vps_embedded_sql_append(&sql, ")", 1U)) goto memory_error;
    sqlite_status = sqlite3_prepare_v3(database->database,
                                       (const char *)sql.data, (int)sql.size,
                                       SQLITE_PREPARE_PERSISTENT,
                                       &database->insert_statement, NULL);
    if (sqlite_status != SQLITE_OK) goto sqlite_error;
    database->column_count = schema->column_count;
    database->index_count = schema->index_count;
    database->source_fingerprint = schema->source_fingerprint;
    database->layout_fingerprint = schema->layout_fingerprint;
    /* Index definitions are created during seal after all row inserts. */
    for (index = 0U; index < schema->index_count; ++index) {
        const VpsEmbeddedIndexDefinition *definition = &schema->indexes[index];
        size_t column;
        if (definition->column_count == 0U ||
            definition->column_count > VPS_EMBEDDED_MAX_INDEX_COLUMNS) {
            vps_buffer_reset(&sql);
            (void)vps_embedded_sqlite_abort(database);
            return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
        }
        for (column = 0U; column < definition->column_count; ++column) {
            if (definition->columns[column] >= schema->column_count) {
                vps_buffer_reset(&sql);
                (void)vps_embedded_sqlite_abort(database);
                return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
            }
        }
    }
    /* Keep an internal schema table for index ordinals, never source values. */
    sqlite_status = sqlite3_exec(database->database,
        "CREATE TABLE vps_index_schema(index_no INTEGER,column_no INTEGER,"
        "ordinal INTEGER,PRIMARY KEY(index_no,ordinal)) WITHOUT ROWID", NULL,
        NULL, NULL);
    if (sqlite_status == SQLITE_OK) {
        sqlite3_stmt *schema_insert = NULL;
        sqlite_status = sqlite3_prepare_v2(database->database,
            "INSERT INTO vps_index_schema VALUES(?,?,?)", -1,
            &schema_insert, NULL);
        for (index = 0U; sqlite_status == SQLITE_OK &&
                         index < schema->index_count; ++index) {
            size_t column;
            for (column = 0U;
                 sqlite_status == SQLITE_OK &&
                 column < schema->indexes[index].column_count; ++column) {
                sqlite_status = sqlite3_bind_int64(schema_insert, 1,
                                                   (sqlite3_int64)index);
                if (sqlite_status == SQLITE_OK)
                    sqlite_status = sqlite3_bind_int64(
                        schema_insert, 2,
                        (sqlite3_int64)schema->indexes[index].columns[column]);
                if (sqlite_status == SQLITE_OK)
                    sqlite_status = sqlite3_bind_int64(schema_insert, 3,
                                                       (sqlite3_int64)column);
                if (sqlite_status == SQLITE_OK)
                    sqlite_status = sqlite3_step(schema_insert) == SQLITE_DONE
                                        ? SQLITE_OK : SQLITE_ERROR;
                (void)sqlite3_reset(schema_insert);
                (void)sqlite3_clear_bindings(schema_insert);
            }
        }
        (void)sqlite3_finalize(schema_insert);
    }
    vps_buffer_reset(&sql);
    if (sqlite_status != SQLITE_OK) {
        (void)vps_embedded_sqlite_abort(database);
        return vps_embedded_status_from_sqlite(sqlite_status);
    }
    return VPS_EMBEDDED_SQLITE_OK;
memory_error:
    vps_buffer_reset(&sql);
    (void)vps_embedded_sqlite_abort(database);
    return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
sqlite_error:
    vps_buffer_reset(&sql);
    (void)vps_embedded_sqlite_abort(database);
    return vps_embedded_status_from_sqlite(sqlite_status);
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_append_row(
    VpsEmbeddedSqlite *database,
    const VpsEmbeddedValue *values,
    size_t value_count)
{
    size_t index;
    uint64_t row_bytes = 0U;
    int sqlite_status = SQLITE_OK;
    if (database == NULL || values == NULL || !database->transaction_active ||
        database->insert_statement == NULL || database->sealed ||
        value_count != database->column_count)
        return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    for (index = 0U; index < value_count; ++index) {
        if (values[index].length > UINT64_MAX - row_bytes)
            return VPS_EMBEDDED_SQLITE_LIMIT_EXCEEDED;
        row_bytes += (uint64_t)values[index].length;
        sqlite_status = vps_embedded_bind(database->insert_statement,
                                          (int)index + 1, &values[index]);
        if (sqlite_status != SQLITE_OK) break;
    }
    if (sqlite_status == SQLITE_OK)
        sqlite_status = sqlite3_step(database->insert_statement) == SQLITE_DONE
                            ? SQLITE_OK : SQLITE_ERROR;
    (void)sqlite3_reset(database->insert_statement);
    (void)sqlite3_clear_bindings(database->insert_statement);
    if (sqlite_status != SQLITE_OK)
        return vps_embedded_status_from_sqlite(sqlite_status);
    if (database->row_count == UINT64_MAX ||
        row_bytes > UINT64_MAX - database->byte_count)
        return VPS_EMBEDDED_SQLITE_LIMIT_EXCEEDED;
    database->row_count += 1U;
    database->byte_count += row_bytes;
    return VPS_EMBEDDED_SQLITE_OK;
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_seal(VpsEmbeddedSqlite *database)
{
    VpsBuffer sql;
    size_t index;
    int sqlite_status = SQLITE_OK;
    if (database == NULL || !database->transaction_active || database->sealed)
        return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    if (database->insert_statement != NULL) {
        sqlite_status = sqlite3_finalize(database->insert_statement);
        database->insert_statement = NULL;
    }
    if (vps_buffer_init(&sql, &database->allocator, 65536U) != VPS_MEMORY_OK) {
        (void)vps_embedded_sqlite_abort(database);
        return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
    }
    for (index = 0U; sqlite_status == SQLITE_OK &&
                     index < database->index_count; ++index) {
        sqlite3_stmt *columns = NULL;
        int row_status = SQLITE_DONE;
        size_t ordinal = 0U;
        sql.size = 0U;
        if (!vps_embedded_sql_append(&sql, "CREATE INDEX vps_idx_", 21U) ||
            !vps_embedded_sql_number(&sql, "", index) ||
            !vps_embedded_sql_append(&sql, " ON vps_snapshot(", 17U)) {
            sqlite_status = SQLITE_NOMEM;
            break;
        }
        sqlite_status = sqlite3_prepare_v2(database->database,
            "SELECT column_no FROM vps_index_schema WHERE index_no=? "
            "ORDER BY ordinal", -1, &columns, NULL);
        if (sqlite_status == SQLITE_OK)
            sqlite_status = sqlite3_bind_int64(columns, 1,
                                               (sqlite3_int64)index);
        while (sqlite_status == SQLITE_OK &&
               (row_status = sqlite3_step(columns)) == SQLITE_ROW) {
            if (sqlite3_column_int(columns, 0) < 0 ||
                (ordinal != 0U &&
                 !vps_embedded_sql_append(&sql, ",", 1U)) ||
                 !vps_embedded_sql_number(
                     &sql, "c", (size_t)sqlite3_column_int(columns, 0))) {
                sqlite_status = SQLITE_NOMEM;
                break;
            }
            ordinal += 1U;
        }
        if (sqlite_status == SQLITE_OK && row_status != SQLITE_DONE)
            sqlite_status = row_status;
        (void)sqlite3_finalize(columns);
        if (sqlite_status == SQLITE_OK &&
            !vps_embedded_sql_append(&sql, ")\0", 2U))
            sqlite_status = SQLITE_NOMEM;
        if (sqlite_status == SQLITE_OK)
            sqlite_status = sqlite3_exec(database->database,
                                         (const char *)sql.data,
                                         NULL, NULL, NULL);
    }
    vps_buffer_reset(&sql);
    if (sqlite_status == SQLITE_OK)
        sqlite_status = sqlite3_exec(database->database, "COMMIT", NULL,
                                     NULL, NULL);
    if (sqlite_status != SQLITE_OK) {
        (void)vps_embedded_sqlite_abort(database);
        return vps_embedded_status_from_sqlite(sqlite_status);
    }
    database->transaction_active = 0;
    sqlite_status = sqlite3_exec(database->database, "PRAGMA query_only=ON",
                                 NULL, NULL, NULL);
    if (sqlite_status != SQLITE_OK)
        return vps_embedded_status_from_sqlite(sqlite_status);
    database->sealed = 1;
    return VPS_EMBEDDED_SQLITE_OK;
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_abort(VpsEmbeddedSqlite *database)
{
    if (database == NULL) return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    if (database->insert_statement != NULL) {
        (void)sqlite3_finalize(database->insert_statement);
        database->insert_statement = NULL;
    }
    if (database->transaction_active && database->database != NULL)
        (void)sqlite3_exec(database->database, "ROLLBACK", NULL, NULL, NULL);
    database->transaction_active = 0;
    database->column_count = 0U;
    database->index_count = 0U;
    database->row_count = 0U;
    database->byte_count = 0U;
    return VPS_EMBEDDED_SQLITE_OK;
}

static const char *vps_embedded_operator_sql(VpsEmbeddedOperator operation)
{
    switch (operation) {
        case VPS_EMBEDDED_OP_EQ: return "=?";
        case VPS_EMBEDDED_OP_NE: return "<>?";
        case VPS_EMBEDDED_OP_LT: return "<?";
        case VPS_EMBEDDED_OP_LE: return "<=?";
        case VPS_EMBEDDED_OP_GT: return ">?";
        case VPS_EMBEDDED_OP_GE: return ">=?";
        case VPS_EMBEDDED_OP_IS_NULL: return " IS NULL";
        case VPS_EMBEDDED_OP_IS_NOT_NULL: return " IS NOT NULL";
        default: return NULL;
    }
}

static VpsEmbeddedSqliteStatus vps_embedded_build_scan_sql(
    VpsEmbeddedSqlite *database,
    const VpsEmbeddedScanRequest *request,
    int explain,
    VpsBuffer *sql)
{
    size_t index;
    if (vps_buffer_init(sql, &database->allocator, 65536U) != VPS_MEMORY_OK)
        return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
    if (explain && !vps_embedded_sql_append(sql, "EXPLAIN QUERY PLAN ", 19U))
        goto memory_error;
    if (!vps_embedded_sql_append(sql, "SELECT ", 7U)) goto memory_error;
    for (index = 0U; index < request->projection_count; ++index) {
        if (request->projection[index] >= database->column_count ||
            (index != 0U && !vps_embedded_sql_append(sql, ",", 1U)) ||
            !vps_embedded_sql_number(sql, "c", request->projection[index]))
            goto invalid;
    }
    if (!vps_embedded_sql_append(sql, " FROM vps_snapshot", 18U))
        goto memory_error;
    if (request->use_index) {
        if (request->selected_index >= database->index_count ||
            !vps_embedded_sql_append(sql, " INDEXED BY vps_idx_", 20U) ||
            !vps_embedded_sql_number(sql, "", request->selected_index))
            goto invalid;
    }
    for (index = 0U; index < request->constraint_count; ++index) {
        const char *operator_sql =
            vps_embedded_operator_sql(request->constraints[index].operation);
        if (request->constraints[index].column >= database->column_count ||
            operator_sql == NULL ||
            !vps_embedded_sql_append(sql, index == 0U ? " WHERE " : " AND ",
                                     index == 0U ? 7U : 5U) ||
            !vps_embedded_sql_number(
                sql, "c", request->constraints[index].column) ||
            !vps_embedded_sql_append(sql, operator_sql, strlen(operator_sql)))
            goto invalid;
    }
    if (request->order_count != 0U) {
        if (!vps_embedded_sql_append(sql, " ORDER BY ", 10U))
            goto memory_error;
        for (index = 0U; index < request->order_count; ++index) {
            if (request->order_terms[index].column >= database->column_count ||
                (index != 0U && !vps_embedded_sql_append(sql, ",", 1U)) ||
                !vps_embedded_sql_number(
                    sql, "c", request->order_terms[index].column) ||
                !vps_embedded_sql_append(
                    sql, request->order_terms[index].descending ? " DESC" :
                                                                   " ASC",
                    4U))
                goto invalid;
        }
    }
    if (request->has_limit) {
        if (!vps_embedded_sql_number(sql, " LIMIT ",
                                     (size_t)request->limit))
            goto memory_error;
    } else if (request->has_offset) {
        if (!vps_embedded_sql_append(sql, " LIMIT -1", 9U))
            goto memory_error;
    }
    if (request->has_offset &&
        !vps_embedded_sql_number(sql, " OFFSET ", (size_t)request->offset))
        goto memory_error;
    return VPS_EMBEDDED_SQLITE_OK;
invalid:
    vps_buffer_reset(sql);
    return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
memory_error:
    vps_buffer_reset(sql);
    return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_scan_open(
    VpsEmbeddedSqlite *database,
    const VpsEmbeddedScanRequest *request,
    VpsEmbeddedSqliteScan **scan)
{
    VpsEmbeddedSqliteScan *candidate = NULL;
    VpsBuffer sql;
    sqlite3_stmt *explain = NULL;
    int sqlite_status;
    size_t index;
    int parameter = 1;
    if (database == NULL || request == NULL || scan == NULL || *scan != NULL ||
        !database->sealed || request->projection == NULL ||
        request->projection_count == 0U ||
        request->projection_count > database->column_count ||
        request->constraint_count > VPS_EMBEDDED_MAX_CONSTRAINTS ||
        request->order_count > VPS_EMBEDDED_MAX_ORDER_TERMS ||
        (request->constraint_count != 0U && request->constraints == NULL) ||
        (request->order_count != 0U && request->order_terms == NULL) ||
        request->limit > SIZE_MAX || request->offset > SIZE_MAX)
        return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    if (vps_memory_allocate(&database->allocator, sizeof(*candidate),
                            (void **)&candidate) != VPS_MEMORY_OK)
        return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
    (void)memset(candidate, 0, sizeof(*candidate));
    candidate->owner = database;
    if (vps_embedded_build_scan_sql(database, request, 1, &sql) !=
        VPS_EMBEDDED_SQLITE_OK) goto memory_error;
    sqlite_status = sqlite3_prepare_v2(database->database,
                                       (const char *)sql.data, (int)sql.size,
                                       &explain, NULL);
    if (sqlite_status == SQLITE_OK && request->use_index) {
        while (sqlite3_step(explain) == SQLITE_ROW) {
            const unsigned char *detail = sqlite3_column_text(explain, 3);
            if (detail != NULL && strstr((const char *)detail,
                                         "vps_idx_") != NULL) {
                candidate->uses_index = 1;
                break;
            }
        }
    }
    (void)sqlite3_finalize(explain);
    vps_buffer_reset(&sql);
    if (sqlite_status != SQLITE_OK) goto sqlite_error;
    if (vps_embedded_build_scan_sql(database, request, 0, &sql) !=
        VPS_EMBEDDED_SQLITE_OK) goto memory_error;
    sqlite_status = sqlite3_prepare_v3(database->database,
                                       (const char *)sql.data, (int)sql.size,
                                       SQLITE_PREPARE_PERSISTENT,
                                       &candidate->statement, NULL);
    vps_buffer_reset(&sql);
    if (sqlite_status != SQLITE_OK) goto sqlite_error;
    for (index = 0U; index < request->constraint_count; ++index) {
        VpsEmbeddedOperator operation = request->constraints[index].operation;
        if (operation == VPS_EMBEDDED_OP_IS_NULL ||
            operation == VPS_EMBEDDED_OP_IS_NOT_NULL)
            continue;
        sqlite_status = vps_embedded_bind(candidate->statement, parameter++,
                                          &request->constraints[index].value);
        if (sqlite_status != SQLITE_OK) goto sqlite_error;
    }
    *scan = candidate;
    return VPS_EMBEDDED_SQLITE_OK;
memory_error:
    vps_memory_release(&database->allocator, (void **)&candidate,
                       sizeof(*candidate));
    return VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY;
sqlite_error:
    if (candidate->statement != NULL)
        (void)sqlite3_finalize(candidate->statement);
    vps_memory_release(&database->allocator, (void **)&candidate,
                       sizeof(*candidate));
    return vps_embedded_status_from_sqlite(sqlite_status);
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_scan_step(
    VpsEmbeddedSqliteScan *scan,
    int *has_row)
{
    int sqlite_status;
    if (scan == NULL || scan->statement == NULL || has_row == NULL)
        return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    sqlite_status = sqlite3_step(scan->statement);
    if (sqlite_status == SQLITE_ROW) {
        scan->has_row = 1;
        *has_row = 1;
        return VPS_EMBEDDED_SQLITE_OK;
    }
    scan->has_row = 0;
    *has_row = 0;
    return sqlite_status == SQLITE_DONE ? VPS_EMBEDDED_SQLITE_OK
                                        : vps_embedded_status_from_sqlite(
                                              sqlite_status);
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_scan_column(
    const VpsEmbeddedSqliteScan *scan,
    size_t column,
    VpsEmbeddedValue *value)
{
    int sqlite_type;
    if (scan == NULL || scan->statement == NULL || !scan->has_row ||
        value == NULL || column >= (size_t)sqlite3_column_count(scan->statement))
        return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    (void)memset(value, 0, sizeof(*value));
    sqlite_type = sqlite3_column_type(scan->statement, (int)column);
    switch (sqlite_type) {
        case SQLITE_NULL: value->kind = VPS_EMBEDDED_VALUE_NULL; break;
        case SQLITE_INTEGER:
            value->kind = VPS_EMBEDDED_VALUE_INTEGER;
            value->integer = (int64_t)sqlite3_column_int64(scan->statement,
                                                           (int)column);
            break;
        case SQLITE_FLOAT:
            value->kind = VPS_EMBEDDED_VALUE_REAL;
            value->real = sqlite3_column_double(scan->statement, (int)column);
            break;
        case SQLITE_TEXT:
            value->kind = VPS_EMBEDDED_VALUE_TEXT;
            value->bytes = sqlite3_column_text(scan->statement, (int)column);
            value->length = (size_t)sqlite3_column_bytes(scan->statement,
                                                         (int)column);
            break;
        case SQLITE_BLOB:
            value->kind = VPS_EMBEDDED_VALUE_BLOB;
            value->bytes = sqlite3_column_blob(scan->statement, (int)column);
            value->length = (size_t)sqlite3_column_bytes(scan->statement,
                                                         (int)column);
            break;
        default: return VPS_EMBEDDED_SQLITE_ERROR;
    }
    return VPS_EMBEDDED_SQLITE_OK;
}

VpsEmbeddedSqliteStatus vps_embedded_sqlite_scan_close(
    VpsEmbeddedSqliteScan **scan)
{
    VpsEmbeddedSqliteScan *owned;
    int sqlite_status = SQLITE_OK;
    if (scan == NULL) return VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT;
    owned = *scan;
    if (owned == NULL) return VPS_EMBEDDED_SQLITE_OK;
    *scan = NULL;
    if (owned->statement != NULL)
        sqlite_status = sqlite3_finalize(owned->statement);
    vps_memory_release(&owned->owner->allocator, (void **)&owned,
                       sizeof(*owned));
    return vps_embedded_status_from_sqlite(sqlite_status);
}

int vps_embedded_sqlite_scan_uses_index(const VpsEmbeddedSqliteScan *scan)
{
    return scan != NULL && scan->uses_index;
}

uint64_t vps_embedded_sqlite_row_count(const VpsEmbeddedSqlite *database)
{
    return database != NULL ? database->row_count : 0U;
}

uint64_t vps_embedded_sqlite_byte_count(const VpsEmbeddedSqlite *database)
{
    return database != NULL ? database->byte_count : 0U;
}

uint64_t vps_embedded_sqlite_source_fingerprint(
    const VpsEmbeddedSqlite *database)
{
    return database != NULL ? database->source_fingerprint : 0U;
}

uint64_t vps_embedded_sqlite_layout_fingerprint(
    const VpsEmbeddedSqlite *database)
{
    return database != NULL ? database->layout_fingerprint : 0U;
}

int vps_embedded_sqlite_version_number(void)
{
    return sqlite3_libversion_number();
}

const char *vps_embedded_sqlite_version(void)
{
    return sqlite3_libversion();
}

uint64_t vps_embedded_sqlite_compile_options_fingerprint(void)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    int index;
    for (index = 0;; ++index) {
        const char *option = sqlite3_compileoption_get(index);
        if (option == NULL) break;
        hash = vps_embedded_hash_bytes(hash, option, strlen(option));
        hash = vps_embedded_hash_bytes(hash, "\0", 1U);
    }
    return hash;
}

const char *vps_embedded_sqlite_status_name(VpsEmbeddedSqliteStatus status)
{
    switch (status) {
        case VPS_EMBEDDED_SQLITE_OK: return "ok";
        case VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT: return "invalid_argument";
        case VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY: return "out_of_memory";
        case VPS_EMBEDDED_SQLITE_LIMIT_EXCEEDED: return "limit_exceeded";
        case VPS_EMBEDDED_SQLITE_BUSY: return "busy";
        case VPS_EMBEDDED_SQLITE_IO_ERROR: return "io_error";
        case VPS_EMBEDDED_SQLITE_ERROR: return "error";
        default: return "unknown";
    }
}
