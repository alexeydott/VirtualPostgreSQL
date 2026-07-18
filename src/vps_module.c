#include "vps_module.h"
SQLITE_EXTENSION_INIT3

#include "vps_arguments.h"
#include "vps_cancel.h"
#include "vps_connection_string.h"
#include "vps_connection_pool.h"
#include "vps_cursor.h"
#include "vps_identity.h"
#include "vps_libpq_client.h"
#include "vps_libpq_client_conninfo.h"
#include "vps_planner.h"
#include "vps_query_validation.h"
#include "vps_query_metadata.h"
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
#include "vps_query_cache.h"
#include "vps_query_indexes.h"
#include "vps_temp_file.h"
#endif
#include "vps_row_identity.h"
#include "vps_session.h"
#include "vps_tls_policy.h"
#include "vps_type_codec.h"
#if defined(_WIN32)
#include "vps_wincred_provider.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VPS_VTAB_QUERY_LIMIT (1024U * 1024U)
#define VPS_VTAB_DRIVE_LIMIT 65536U
#define VPS_VTAB_PARAMETER_LIMIT (VPS_PLAN_DEFAULT_IN_LIMIT + VPS_PLAN_MAX_CONSTRAINTS)
#define VPS_VTAB_PARAMETER_BYTES_LIMIT (4U * 1024U * 1024U)

typedef struct VpsTable {
    sqlite3_vtab base;
    sqlite3 *database;
    VpsAllocator allocator;
    VpsLogger logger;
#if defined(_WIN32)
    VpsCredentialRegistry credential_registry;
    VpsWinCredProviderContext wincred;
#endif
    VpsParsedArguments arguments;
    VpsConnectionConfig connection_config;
    VpsConnectionIdentity identity;
    VpsTlsPolicy tls_policy;
    VpsSessionPlan session_plan;
    VpsLibpqClient adapter;
    VpsClient client;
    VpsConnectionPool *pool;
    VpsConnectionPoolKey pool_key;
    VpsQueryDescribeResult described;
    VpsClientResultFieldExpectation *expected_fields;
    size_t expected_fields_size;
    VpsBuffer scan_query;
    uint64_t source_fingerprint;
    VpsModuleContext *module_context;
    uint16_t key_columns[VPS_QUERY_METADATA_MAX_KEY_COLUMNS];
    size_t key_column_count;
    VpsRowIdentityMode identity_mode;
    uint64_t table_id;
    uint64_t next_cursor_id;
    size_t active_cursors;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    VpsQueryIndexSet query_indexes;
    VpsQueryCache *query_cache;
    VpsTempFilePath temp_path;
    VpsQueryMaterializationMode materialization;
    int source_is_query;
#endif
    int initialized_arguments;
#if defined(_WIN32)
    int initialized_registry;
    int initialized_wincred;
#endif
    int initialized_config;
    int initialized_identity;
    int initialized_adapter;
    int initialized_client;
    int initialized_pool;
    int initialized_described;
    int initialized_query;
} VpsTable;

typedef struct VpsCursor {
    sqlite3_vtab_cursor base;
    VpsTable *table;
    VpsClientConnection *connection;
    VpsConnectionLease lease;
    VpsClientStatement *statement;
    const VpsClientRowView *row;
    VpsCompiledPlan plan;
    VpsBuffer planned_query;
    VpsBuffer parameter_bytes;
    VpsClientParameterView *parameters;
    size_t parameters_size;
    size_t parameter_count;
    VpsClientResultFieldExpectation *projected_fields;
    size_t projected_fields_size;
    uint16_t logical_to_remote[VPS_PLAN_MAX_COLUMNS];
    VpsDecodedValue decoded[VPS_PLAN_MAX_COLUMNS];
    VpsBuffer identity_token;
    int initialized_planned_query;
    int initialized_parameter_bytes;
    int initialized_identity_token;
    VpsCursorMachine machine;
    VpsCancelToken cancel_token;
    VpsCursorBudget budget;
    uint64_t cursor_id;
    uint64_t scan_counter;
    int64_t rowid;
    size_t row_bytes;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    VpsQueryCacheLease snapshot_lease;
    VpsEmbeddedSqliteScan *snapshot_scan;
    VpsEmbeddedValue snapshot_values[VPS_PLAN_MAX_COLUMNS];
    int snapshot_row;
#endif
} VpsCursor;

typedef struct VpsEmptyTable { sqlite3_vtab base; } VpsEmptyTable;
typedef struct VpsEmptyCursor {
    sqlite3_vtab_cursor base;
    int is_eof;
} VpsEmptyCursor;

static VpsConnectionPoolResult vps_vtab_pool_create(void *context,
                                                     void **connection_out);
static VpsConnectionPoolResult vps_vtab_pool_ready(void *context,
                                                    void *connection);
static void vps_vtab_pool_destroy(void *context, void *connection);
static int vps_vtab_uuid_canonical(const unsigned char *value, size_t length);

static VpsInterruptProbeResult vps_cursor_interrupt_probe(void *context)
{
    VpsCursor *cursor = (VpsCursor *)context;
    if (cursor == NULL || cursor->table == NULL ||
        cursor->table->database == NULL)
        return VPS_INTERRUPT_PROBE_ERROR;
    if (sqlite3_is_interrupted(cursor->table->database))
        return VPS_INTERRUPT_REQUESTED;
    return vps_cancel_token_probe(&cursor->cancel_token);
}

VpsModuleContext *vps_module_context_create(sqlite3 *database)
{
    VpsModuleContext *context =
        (VpsModuleContext *)sqlite3_malloc64(sizeof(*context));
    if (context == NULL || database == NULL) {
        sqlite3_free(context);
        return NULL;
    }
    (void)memset(context, 0, sizeof(*context));
    context->database = database;
    if (vps_cancel_registry_init(&context->cancel_registry,
                                 vps_platform_current_operations(), NULL) !=
        VPS_CANCEL_REGISTRY_OK) {
        sqlite3_free(context);
        return NULL;
    }
    context->initialized_cancel_registry = 1;
    return context;
}

void vps_module_context_destroy(void *opaque)
{
    VpsModuleContext *context = (VpsModuleContext *)opaque;
    if (context == NULL) return;
    context->closing = 1;
    if (context->initialized_cancel_registry)
        (void)vps_cancel_registry_cleanup(&context->cancel_registry);
    sqlite3_free(context);
}

static int vps_vtab_log_sink(void *context, const VpsLogEvent *event)
{
    char operation[VPS_LOG_MAX_STRING_LENGTH + 1U] = "unknown";
    char phase[VPS_LOG_MAX_STRING_LENGTH + 1U] = "unknown";
    char status[VPS_LOG_MAX_STRING_LENGTH + 1U] = "unknown";
    char sqlstate[VPS_SQLSTATE_BUFFER_SIZE] = "";
    char primary[VPS_LOG_MAX_STRING_LENGTH + 1U] = "";
    size_t index;
    (void)context;
    if (event == NULL) return 0;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        char *destination = NULL;
        size_t capacity = 0U;
        if (field->type != VPS_LOG_FIELD_TYPE_STRING) continue;
        switch (field->key) {
            case VPS_LOG_FIELD_OPERATION:
                destination = operation; capacity = sizeof(operation); break;
            case VPS_LOG_FIELD_PHASE:
                destination = phase; capacity = sizeof(phase); break;
            case VPS_LOG_FIELD_STATUS:
                destination = status; capacity = sizeof(status); break;
            case VPS_LOG_FIELD_SQLSTATE:
                destination = sqlstate; capacity = sizeof(sqlstate); break;
            case VPS_LOG_FIELD_PRIMARY_MESSAGE:
                destination = primary; capacity = sizeof(primary); break;
            default: break;
        }
        if (destination != NULL && capacity != 0U) {
            size_t length = field->value.string_value.length;
            if (length >= capacity) length = capacity - 1U;
            (void)memcpy(destination, field->value.string_value.data, length);
            destination[length] = '\0';
        }
    }
    sqlite3_log(event->level >= VPS_LOG_LEVEL_ERROR ? SQLITE_ERROR
                                                     : SQLITE_NOTICE,
                "VirtualPostgreSQL operation=%s phase=%s status=%s sqlstate=%s primary=%s",
                operation, phase, status, sqlstate, primary);
    return 0;
}

static VpsLogLevel vps_vtab_log_level(void)
{
    char buffer[32];
    size_t required = 0U;
    VpsLogLevel level = VPS_LOG_LEVEL_INFO;
    if (vps_platform_environment_get(vps_platform_current_operations(),
                                     "VPS_LOG_LEVEL", buffer,
                                     sizeof(buffer), &required) ==
            VPS_PLATFORM_OK &&
        required <= sizeof(buffer))
        (void)vps_log_level_parse(buffer, &level);
    return level;
}

static int vps_vtab_set_error(sqlite3_vtab *table,
                              int sqlite_code,
                              const VpsError *error,
                              const char *fallback)
{
    const char *message = fallback;
    if (table != NULL) {
        sqlite3_free(table->zErrMsg);
        table->zErrMsg = NULL;
        if (error != NULL && vps_error_message(error) != NULL &&
            vps_error_message(error)[0] != '\0')
            message = vps_error_message(error);
        table->zErrMsg = sqlite3_mprintf("%s", message);
        if (table->zErrMsg == NULL) return SQLITE_NOMEM;
    }
    return sqlite_code;
}

static VpsClientStatus vps_drive_connection(VpsClientConnection *connection,
                                            VpsError *error)
{
    size_t attempt;
    for (attempt = 0U; attempt < VPS_VTAB_DRIVE_LIMIT; ++attempt) {
        VpsClientPollResult poll;
        VpsClientStatus status =
            vps_client_connection_poll(connection, &poll, error);
        if (status != VPS_CLIENT_OK) return status;
        if (poll.outcome == VPS_CLIENT_POLL_COMPLETE) return VPS_CLIENT_OK;
        if (poll.outcome != VPS_CLIENT_POLL_WAIT)
            return VPS_CLIENT_BACKEND_ERROR;
        status = vps_client_connection_wait(connection, &poll.wait, error);
        if (status != VPS_CLIENT_OK) return status;
    }
    return VPS_CLIENT_LIMIT_EXCEEDED;
}

static VpsConnectionPoolResult vps_vtab_pool_create(void *context,
                                                     void **connection_out)
{
    VpsTable *table = (VpsTable *)context;
    VpsClientConnection *connection = NULL;
    VpsError error;
    VpsClientStatus status;
    if (table == NULL || connection_out == NULL ||
        vps_error_init(&error, &table->allocator) != VPS_MEMORY_OK)
        return VPS_CONNECTION_POOL_CREATE_FAILED;
    status = vps_client_connection_open(&table->client, &connection, &error);
    if (status == VPS_CLIENT_OK)
        status = vps_client_connection_start(
            connection, VPS_CLIENT_OPERATION_CONNECT, &error);
    if (status == VPS_CLIENT_OK) status = vps_drive_connection(connection, &error);
    vps_error_reset(&error);
    if (status != VPS_CLIENT_OK) {
        if (connection != NULL)
            (void)vps_client_connection_close(&connection);
        return VPS_CONNECTION_POOL_CREATE_FAILED;
    }
    *connection_out = connection;
    return VPS_CONNECTION_POOL_OK;
}

static VpsConnectionPoolResult vps_vtab_pool_ready(void *context,
                                                    void *connection)
{
    (void)context;
    return connection != NULL &&
                   vps_client_connection_state(
                       (VpsClientConnection *)connection) ==
                       VPS_CLIENT_CONNECTION_READY
               ? VPS_CONNECTION_POOL_OK
               : VPS_CONNECTION_POOL_VALIDATE_FAILED;
}

static void vps_vtab_pool_destroy(void *context, void *connection)
{
    VpsClientConnection *owned = (VpsClientConnection *)connection;
    (void)context;
    if (owned != NULL) (void)vps_client_connection_close(&owned);
}

static VpsClientStatus vps_drive_statement(VpsClientStatement *statement,
                                           VpsCursorMachine *machine,
                                           VpsError *error)
{
    size_t attempt;
    for (attempt = 0U; attempt < VPS_VTAB_DRIVE_LIMIT; ++attempt) {
        VpsClientPollResult poll;
        VpsClientStatus status =
            vps_client_statement_poll(statement, &poll, error);
        if (status != VPS_CLIENT_OK) return status;
        if (poll.outcome == VPS_CLIENT_POLL_COMPLETE ||
            poll.outcome == VPS_CLIENT_POLL_ROW_READY) {
            if (machine != NULL && machine->state == VPS_CURSOR_WAITING &&
                vps_cursor_transition(machine, VPS_CURSOR_EVENT_RESUME) !=
                    VPS_CURSOR_OK)
                return VPS_CLIENT_INVALID_STATE;
            return VPS_CLIENT_OK;
        }
        if (poll.outcome != VPS_CLIENT_POLL_WAIT)
            return VPS_CLIENT_BACKEND_ERROR;
        if (machine != NULL &&
            vps_cursor_transition(machine, VPS_CURSOR_EVENT_WAIT) !=
                VPS_CURSOR_OK)
            return VPS_CLIENT_INVALID_STATE;
        status = vps_client_statement_wait(statement, &poll.wait, error);
        if (status != VPS_CLIENT_OK && machine != NULL && error != NULL &&
            error->initialized &&
            (error->sqlite_code == SQLITE_INTERRUPT ||
             error->error_class == VPS_ERROR_CLASS_TIMEOUT ||
             error->error_class == VPS_ERROR_CLASS_CANCEL)) {
            if (vps_cursor_transition(machine, VPS_CURSOR_EVENT_CANCEL) !=
                    VPS_CURSOR_OK ||
                vps_client_statement_start(statement,
                                           VPS_CLIENT_OPERATION_CANCEL,
                                           error) != VPS_CLIENT_OK)
                return VPS_CLIENT_BACKEND_ERROR;
            continue;
        }
        if (machine != NULL &&
            vps_cursor_transition(machine, VPS_CURSOR_EVENT_RESUME) !=
                VPS_CURSOR_OK)
            return VPS_CLIENT_INVALID_STATE;
        if (status != VPS_CLIENT_OK) return status;
    }
    return VPS_CLIENT_LIMIT_EXCEEDED;
}

static int vps_append_bytes(VpsBuffer *buffer, const char *bytes, size_t length)
{
    return vps_buffer_append(buffer, bytes, length) == VPS_MEMORY_OK;
}

static int vps_append_pg_identifier(VpsBuffer *buffer,
                                    const char *identifier,
                                    size_t length)
{
    size_t index;
    if (identifier == NULL || length == 0U ||
        !vps_append_bytes(buffer, "\"", 1U)) return 0;
    for (index = 0U; index < length; ++index) {
        if (identifier[index] == '\0') return 0;
        if (identifier[index] == '"' &&
            !vps_append_bytes(buffer, "\"", 1U)) return 0;
        if (!vps_append_bytes(buffer, &identifier[index], 1U)) return 0;
    }
    return vps_append_bytes(buffer, "\"", 1U);
}

static int vps_append_sqlite_identifier(VpsBuffer *buffer,
                                        const char *identifier,
                                        size_t length)
{
    return vps_append_pg_identifier(buffer, identifier, length);
}

static int vps_build_table_query(VpsTable *table)
{
    const char *schema;
    const char *relation;
    size_t schema_length = 0U;
    size_t relation_length = 0U;
    schema = vps_argument_text(&table->arguments, VPS_ARGUMENT_ID_SCHEMA,
                               &schema_length);
    relation = vps_argument_text(&table->arguments, VPS_ARGUMENT_ID_TABLE,
                                 &relation_length);
    if (schema == NULL || relation == NULL ||
        vps_buffer_init(&table->scan_query, &table->allocator,
                        VPS_VTAB_QUERY_LIMIT) != VPS_MEMORY_OK ||
        !vps_append_bytes(&table->scan_query, "SELECT * FROM ", 14U) ||
        !vps_append_pg_identifier(&table->scan_query, schema, schema_length) ||
        !vps_append_bytes(&table->scan_query, ".", 1U) ||
        !vps_append_pg_identifier(&table->scan_query, relation,
                                  relation_length) ||
        !vps_append_bytes(&table->scan_query, "\0", 1U))
        return 0;
    table->scan_query.size -= 1U;
    table->initialized_query = 1;
    return 1;
}

static int vps_build_query_source(VpsTable *table)
{
    const char *query;
    size_t query_length = 0U;
    VpsQuerySourceAnalysis analysis;
    query = vps_argument_text(&table->arguments, VPS_ARGUMENT_ID_QUERY,
                              &query_length);
    if (query == NULL ||
        vps_query_source_scan(query, query_length, &table->logger, &analysis) !=
            VPS_QUERY_SOURCE_OK ||
        vps_buffer_init(&table->scan_query, &table->allocator,
                        VPS_VTAB_QUERY_LIMIT) != VPS_MEMORY_OK ||
        !vps_append_bytes(&table->scan_query, "SELECT * FROM (", 15U) ||
        !vps_append_bytes(&table->scan_query, query,
                          analysis.statement_length) ||
        !vps_append_bytes(&table->scan_query, ") AS vps_scan\0", 14U))
        return 0;
    table->scan_query.size -= 1U;
    table->initialized_query = 1;
    return 1;
}

#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
static VpsQueryMaterializationMode vps_table_materialization_mode(
    const VpsParsedArguments *arguments)
{
    const VpsArgumentValue *value = vps_arguments_get(
        arguments, VPS_ARGUMENT_ID_QUERY_MATERIALIZATION);
    if (value == NULL || !value->present ||
        value->enum_value == VPS_ARGUMENT_ENUM_MATERIALIZATION_OFF)
        return VPS_QUERY_MATERIALIZATION_OFF;
    return value->enum_value == VPS_ARGUMENT_ENUM_MATERIALIZATION_TEMP
               ? VPS_QUERY_MATERIALIZATION_TEMP
               : VPS_QUERY_MATERIALIZATION_MEMORY;
}

static VpsEmbeddedValueKind vps_snapshot_kind(uint32_t type_oid)
{
    switch (vps_type_codec_for_oid(type_oid)) {
        case VPS_CODEC_BOOLEAN: case VPS_CODEC_INTEGER:
            return VPS_EMBEDDED_VALUE_INTEGER;
        case VPS_CODEC_FLOAT: return VPS_EMBEDDED_VALUE_REAL;
        case VPS_CODEC_BYTEA: return VPS_EMBEDDED_VALUE_BLOB;
        default: return VPS_EMBEDDED_VALUE_TEXT;
    }
}

static int vps_table_materialization_init(VpsTable *table)
{
    const char *definition;
    size_t definition_length = 0U;
    VpsQueryCacheConfig config;
    table->materialization =
        vps_table_materialization_mode(&table->arguments);
    definition = vps_argument_text(&table->arguments,
                                   VPS_ARGUMENT_ID_QUERY_INDEXES,
                                   &definition_length);
    if (vps_query_indexes_parse(definition, definition_length,
                                &table->described,
                                &table->query_indexes) !=
        VPS_QUERY_INDEXES_OK) return 0;
    table->source_fingerprint ^= (uint64_t)table->materialization;
    table->source_fingerprint *= UINT64_C(1099511628211);
    {
        size_t index;
        for (index = 0U; index < table->query_indexes.index_count; ++index) {
            size_t column;
            table->source_fingerprint ^=
                table->query_indexes.indexes[index].name_hash;
            table->source_fingerprint *= UINT64_C(1099511628211);
            for (column = 0U;
                 column < table->query_indexes.indexes[index].column_count;
                 ++column) {
                table->source_fingerprint ^=
                    table->query_indexes.indexes[index].columns[column];
                table->source_fingerprint *= UINT64_C(1099511628211);
            }
        }
    }
    if (table->materialization == VPS_QUERY_MATERIALIZATION_OFF) return 1;
    if (!table->source_is_query) return 0;
    (void)memset(&config, 0, sizeof(config));
    config.allocator = table->allocator;
    config.platform = vps_platform_current_operations();
    config.logger = &table->logger;
    config.mode = table->materialization == VPS_QUERY_MATERIALIZATION_TEMP
                      ? VPS_EMBEDDED_SQLITE_TEMP
                      : VPS_EMBEDDED_SQLITE_MEMORY;
    config.source_fingerprint = table->source_fingerprint;
    config.layout_fingerprint = table->source_fingerprint;
    config.wait_slice_ms = VPS_QUERY_CACHE_DEFAULT_WAIT_SLICE_MS;
    if (table->materialization == VPS_QUERY_MATERIALIZATION_TEMP) {
#if defined(_WIN32)
        if (vps_temp_file_create_private(&table->allocator, &table->logger,
                                         &table->temp_path) !=
            VPS_TEMP_FILE_OK) return 0;
        config.temp_path = table->temp_path.path;
        config.temp_path_length = table->temp_path.path_size - 1U;
#else
        return 0;
#endif
    }
    return vps_query_cache_create(&config, &table->query_cache) ==
           VPS_QUERY_CACHE_OK;
}

static VpsQueryCacheStatus vps_table_build_snapshot(void *context,
                                                    VpsEmbeddedSqlite *snapshot)
{
    VpsTable *table = (VpsTable *)context;
    VpsEmbeddedValueKind kinds[VPS_PLAN_MAX_COLUMNS];
    VpsEmbeddedIndexDefinition indexes[VPS_QUERY_INDEX_MAX_COUNT];
    VpsEmbeddedSchema schema;
    VpsConnectionLease lease;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    VpsClientStatementSpec spec;
    VpsCursorLimits limits;
    VpsCursorBudget budget;
    VpsError error;
    VpsClientStatus client_status = VPS_CLIENT_BACKEND_ERROR;
    VpsQueryCacheStatus result = VPS_QUERY_CACHE_BUILD_FAILED;
    int error_initialized = 0;
    int complete = 0;
    size_t index;
    (void)memset(&lease, 0, sizeof(lease));
    (void)memset(indexes, 0, sizeof(indexes));
    vps_cursor_limits_default(&limits);
    if (vps_cursor_budget_init(&budget, &limits, UINT64_C(1), &table->logger) !=
        VPS_CURSOR_LIMIT_OK)
        return VPS_QUERY_CACHE_BUILD_FAILED;
    for (index = 0U; index < table->described.field_count; ++index)
        kinds[index] = vps_snapshot_kind(table->described.fields[index].type_oid);
    for (index = 0U; index < table->query_indexes.index_count; ++index) {
        size_t column;
        indexes[index].column_count =
            table->query_indexes.indexes[index].column_count;
        indexes[index].name_hash = table->query_indexes.indexes[index].name_hash;
        for (column = 0U; column < indexes[index].column_count; ++column)
            indexes[index].columns[column] =
                table->query_indexes.indexes[index].columns[column];
    }
    (void)memset(&schema, 0, sizeof(schema));
    schema.column_kinds = kinds;
    schema.column_count = table->described.field_count;
    schema.indexes = indexes;
    schema.index_count = table->query_indexes.index_count;
    schema.source_fingerprint = table->source_fingerprint;
    schema.layout_fingerprint = table->source_fingerprint;
    if (vps_embedded_sqlite_create_schema(snapshot, &schema) !=
        VPS_EMBEDDED_SQLITE_OK) return VPS_QUERY_CACHE_BUILD_FAILED;
    if (vps_error_init(&error, &table->allocator) == VPS_MEMORY_OK)
        error_initialized = 1;
    if (!error_initialized ||
        vps_connection_pool_acquire(table->pool, &table->pool_key, 10000U,
                                    NULL, NULL, &lease) !=
            VPS_CONNECTION_POOL_OK)
        goto cleanup;
    connection = (VpsClientConnection *)lease.connection;
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = (const char *)table->scan_query.data;
    spec.query_length = table->scan_query.size;
    spec.result_fields = table->expected_fields;
    spec.result_field_count = table->described.field_count;
    spec.timeout_ms = 60000U;
    spec.single_row = 1;
    vps_query_cache_record_remote_execution(table->query_cache);
    client_status = vps_client_statement_open(connection, &spec, &statement,
                                              &error);
    if (client_status == VPS_CLIENT_OK)
        client_status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_EXECUTE, &error);
    if (client_status == VPS_CLIENT_OK)
        client_status = vps_drive_statement(statement, NULL, &error);
    while (client_status == VPS_CLIENT_OK) {
        const VpsClientRowView *row = NULL;
        VpsDecodedValue decoded[VPS_PLAN_MAX_COLUMNS];
        VpsEmbeddedValue values[VPS_PLAN_MAX_COLUMNS];
        uint64_t row_bytes = 0U;
        uint64_t largest_column = 0U;
        size_t column;
        (void)memset(decoded, 0, sizeof(decoded));
        (void)memset(values, 0, sizeof(values));
        client_status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_FETCH, &error);
        if (client_status == VPS_CLIENT_OK)
            client_status = vps_drive_statement(statement, NULL, &error);
        if (client_status != VPS_CLIENT_OK) break;
        if (vps_client_statement_state(statement) ==
            VPS_CLIENT_STATEMENT_COMPLETE) {
            complete = 1;
            break;
        }
        client_status = vps_client_statement_current_row(statement, &row,
                                                         &error);
        if (client_status != VPS_CLIENT_OK ||
            vps_client_row_column_count(row) != table->described.field_count)
            break;
        for (column = 0U; column < table->described.field_count; ++column) {
            VpsClientColumnView source;
            if (vps_client_row_column(row, column, &source, &error) !=
                    VPS_CLIENT_OK ||
                vps_type_codec_decode(
                    &table->allocator, vps_type_codec_for_oid(source.type_oid),
                    &source, &decoded[column]) != VPS_TYPE_CODEC_OK) {
                client_status = VPS_CLIENT_BACKEND_ERROR;
                break;
            }
            values[column].kind =
                (VpsEmbeddedValueKind)decoded[column].kind;
            values[column].bytes = decoded[column].bytes;
            values[column].length = decoded[column].length;
            values[column].integer = decoded[column].integer;
            values[column].real = decoded[column].real;
            if ((uint64_t)decoded[column].length >
                    UINT64_MAX - row_bytes) {
                client_status = VPS_CLIENT_BACKEND_ERROR;
                break;
            }
            row_bytes += (uint64_t)decoded[column].length;
            if ((uint64_t)decoded[column].length > largest_column)
                largest_column = (uint64_t)decoded[column].length;
        }
        if (client_status == VPS_CLIENT_OK &&
            vps_cursor_budget_observe_row(&budget, row_bytes,
                                          largest_column, 0U) !=
                VPS_CURSOR_LIMIT_OK)
            client_status = VPS_CLIENT_BACKEND_ERROR;
        if (client_status == VPS_CLIENT_OK &&
            vps_embedded_sqlite_append_row(
                snapshot, values, table->described.field_count) !=
                VPS_EMBEDDED_SQLITE_OK)
            client_status = VPS_CLIENT_BACKEND_ERROR;
        for (column = 0U; column < table->described.field_count; ++column)
            vps_decoded_value_reset(&decoded[column]);
        if (client_status != VPS_CLIENT_OK) break;
        client_status = vps_client_statement_row_consumed(statement, &error);
    }
    if (client_status == VPS_CLIENT_OK && complete)
        result = VPS_QUERY_CACHE_OK;
cleanup:
    if (statement != NULL) (void)vps_client_statement_close(&statement);
    if (lease.pool != NULL)
        (void)vps_connection_lease_release(
            &lease, complete && client_status == VPS_CLIENT_OK
                        ? VPS_CONNECTION_LEASE_CLEAN
                        : VPS_CONNECTION_LEASE_DIRTY);
    if (error_initialized) vps_error_reset(&error);
    return result;
}
#endif

static int vps_table_cleanup(VpsTable *table)
{
    int clean = 1;
    if (table == NULL) return SQLITE_OK;
    if (table->active_cursors != 0U) return SQLITE_BUSY;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    if (table->query_cache != NULL &&
        vps_query_cache_destroy(&table->query_cache) != VPS_QUERY_CACHE_OK)
        clean = 0;
    if (table->temp_path.path != NULL &&
        vps_temp_file_delete(&table->temp_path) != VPS_TEMP_FILE_OK)
        clean = 0;
#endif
    if (table->initialized_described)
        vps_query_describe_result_cleanup(&table->described);
    vps_memory_release(&table->allocator, (void **)&table->expected_fields,
                       table->expected_fields_size);
    if (table->initialized_query) vps_buffer_reset(&table->scan_query);
    if (table->initialized_pool &&
        vps_connection_pool_destroy(&table->pool) != VPS_CONNECTION_POOL_OK)
        clean = 0;
    if (table->initialized_client &&
        vps_client_cleanup(&table->client) != VPS_CLIENT_OK) clean = 0;
    if (table->initialized_adapter &&
        vps_libpq_client_cleanup(&table->adapter) != VPS_CLIENT_OK) clean = 0;
    if (table->initialized_identity) vps_identity_cleanup(&table->identity);
    if (table->initialized_config &&
        vps_connection_config_cleanup(&table->connection_config) !=
            VPS_CONNECTION_STRING_OK) clean = 0;
#if defined(_WIN32)
    if (table->initialized_registry &&
        vps_credential_registry_cleanup(&table->credential_registry) !=
            VPS_CREDENTIAL_REGISTRY_OK) clean = 0;
    if (table->initialized_wincred &&
        vps_wincred_provider_cleanup(&table->wincred) !=
            VPS_CREDENTIAL_REGISTRY_OK) clean = 0;
#endif
    if (table->initialized_arguments &&
        vps_arguments_reset(&table->arguments) != VPS_ARGUMENTS_OK) clean = 0;
    if (table->module_context != NULL &&
        table->module_context->table_references != 0U)
        table->module_context->table_references -= 1U;
    sqlite3_free(table->base.zErrMsg);
    sqlite3_free(table);
    return clean ? SQLITE_OK : SQLITE_ERROR;
}

static int vps_table_initialize_runtime(VpsTable *table,
                                        int argument_count,
                                        const char *const *arguments,
                                        VpsError *error)
{
    VpsArgumentInput inputs[VPS_ARGUMENT_MAX_COUNT];
    VpsConnectionResolveOptions resolve_options;
    VpsConninfoParser parser;
    VpsIdentityBuildOptions identity_options = {"postgresql", 10U, 1U, 1U};
    VpsTlsPolicyOptions tls_options;
    VpsSessionBuildOptions session_options = {NULL, 0U, 2000U};
    VpsLibpqClientOptions adapter_options;
    VpsClientOperations operations;
    VpsConnectionPoolConfig pool_config;
    const VpsArgumentValue *source;
    size_t index;
    if (argument_count < 4 ||
        (size_t)(argument_count - 3) > VPS_ARGUMENT_MAX_COUNT)
        return SQLITE_MISUSE;
    if (vps_allocator_system(&table->allocator) != VPS_MEMORY_OK ||
        vps_logger_init(&table->logger, vps_vtab_log_level(), vps_vtab_log_sink,
                        NULL) != VPS_LOG_OK ||
        vps_arguments_init(&table->arguments, &table->allocator,
                           vps_platform_current_operations(), &table->logger) !=
            VPS_ARGUMENTS_OK)
        return SQLITE_NOMEM;
    table->initialized_arguments = 1;
    for (index = 3U; index < (size_t)argument_count; ++index) {
        inputs[index - 3U].text = arguments[index];
        inputs[index - 3U].length = strlen(arguments[index]);
    }
    if (vps_arguments_parse(&table->arguments, inputs,
                            (size_t)argument_count - 3U, NULL) !=
        VPS_ARGUMENTS_OK) return SQLITE_ERROR;
#if !defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    {
        const VpsArgumentValue *materialization = vps_arguments_get(
            &table->arguments, VPS_ARGUMENT_ID_QUERY_MATERIALIZATION);
        if (materialization != NULL && materialization->present &&
            materialization->enum_value !=
                VPS_ARGUMENT_ENUM_MATERIALIZATION_OFF)
            return SQLITE_ERROR;
    }
#endif
    if (vps_connection_config_init(&table->connection_config,
                                   &table->allocator,
                                   vps_platform_current_operations(),
                                   &table->logger) != VPS_CONNECTION_STRING_OK)
        return SQLITE_NOMEM;
    table->initialized_config = 1;
    (void)memset(&resolve_options, 0, sizeof(resolve_options));
#if defined(_WIN32)
    {
        VpsCredentialProvider provider;
        if (vps_credential_registry_init(
                &table->credential_registry,
                vps_platform_current_operations(), &table->logger) !=
                VPS_CREDENTIAL_REGISTRY_OK)
            return SQLITE_ERROR;
        table->initialized_registry = 1;
        if (vps_wincred_provider_init(
                &table->wincred, &table->allocator,
                vps_platform_current_operations(), &table->logger) !=
                VPS_CREDENTIAL_REGISTRY_OK)
            return SQLITE_ERROR;
        table->initialized_wincred = 1;
        if (vps_wincred_provider_make(&table->wincred, &provider) !=
                VPS_CREDENTIAL_REGISTRY_OK ||
            vps_credential_registry_register(
                &table->credential_registry, UINT64_C(1), &provider) !=
                VPS_CREDENTIAL_REGISTRY_OK)
            return SQLITE_ERROR;
        resolve_options.credential_registry = &table->credential_registry;
    }
#endif
    parser.context = NULL;
    parser.parse = vps_libpq_client_conninfo_parse;
    resolve_options.conninfo_parser = &parser;
    if (vps_connection_config_resolve(&table->connection_config,
                                      &table->arguments,
                                      &resolve_options) !=
        VPS_CONNECTION_STRING_OK) return SQLITE_AUTH;
    tls_options.allow_explicit_disable =
        table->connection_config.config.sslmode != NULL &&
        strcmp(table->connection_config.config.sslmode, "disable") == 0;
    if (vps_tls_policy_from_config(&table->connection_config.config,
                                   &tls_options, &table->tls_policy) !=
            VPS_TLS_OK ||
        vps_session_plan_init(&table->session_plan, &table->logger) !=
            VPS_SESSION_OK ||
        vps_session_plan_build(&table->session_plan,
                               &table->connection_config, &table->arguments,
                               &session_options) != VPS_SESSION_OK ||
        vps_identity_init(&table->identity, &table->allocator,
                          &table->logger) != VPS_IDENTITY_OK)
        return SQLITE_ERROR;
    table->initialized_identity = 1;
    if (vps_identity_build(&table->identity, &table->connection_config,
                           &table->arguments, &identity_options) !=
        VPS_IDENTITY_OK) return SQLITE_ERROR;
    (void)memset(&adapter_options, 0, sizeof(adapter_options));
    adapter_options.allocator = &table->allocator;
    adapter_options.platform_operations = vps_platform_current_operations();
    adapter_options.connection_config = &table->connection_config;
    adapter_options.identity = &table->identity;
    adapter_options.tls_policy = &table->tls_policy;
    adapter_options.session_plan = &table->session_plan;
    adapter_options.logger = &table->logger;
    adapter_options.connect_timeout_ms = 10000U;
    adapter_options.cancel_timeout_ms = 5000U;
    adapter_options.wait_slice_ms = 50U;
    if (vps_libpq_client_init(&table->adapter, &adapter_options) !=
        VPS_CLIENT_OK) return SQLITE_ERROR;
    table->initialized_adapter = 1;
    if (vps_libpq_client_make_operations(&table->adapter, &operations) !=
            VPS_CLIENT_OK ||
        vps_client_init(&table->client, &table->allocator, &operations,
                        &table->adapter, &table->logger) != VPS_CLIENT_OK)
        return SQLITE_ERROR;
    table->initialized_client = 1;
    (void)memset(&table->pool_key, 0, sizeof(table->pool_key));
    table->pool_key.identity = table->identity.canonical.data;
    table->pool_key.identity_size = table->identity.canonical.size;
    table->pool_key.fingerprint = vps_identity_fingerprint(&table->identity);
    table->pool_key.credential_generation = table->identity.credential_generation;
    table->pool_key.configuration_generation =
        table->identity.configuration_generation;
    table->pool_key.read_only = 1;
    (void)memset(&pool_config, 0, sizeof(pool_config));
    pool_config.allocator = table->allocator;
    pool_config.platform = vps_platform_current_operations();
    pool_config.logger = &table->logger;
    pool_config.key = table->pool_key;
    pool_config.maximum_size = 8U;
    pool_config.maximum_waiters = 64U;
    pool_config.wait_slice_ms = 50U;
    pool_config.callbacks.context = table;
    pool_config.callbacks.create = vps_vtab_pool_create;
    pool_config.callbacks.validate = vps_vtab_pool_ready;
    pool_config.callbacks.reset = vps_vtab_pool_ready;
    pool_config.callbacks.destroy = vps_vtab_pool_destroy;
    if (vps_connection_pool_create(&pool_config, &table->pool) !=
        VPS_CONNECTION_POOL_OK) return SQLITE_ERROR;
    table->initialized_pool = 1;
    source = vps_arguments_get(&table->arguments, VPS_ARGUMENT_ID_SOURCE);
    if (source == NULL || !source->present) return SQLITE_ERROR;
    if (source->enum_value == VPS_ARGUMENT_ENUM_SOURCE_TABLE) {
        if (!vps_build_table_query(table)) return SQLITE_ERROR;
    } else if (source->enum_value == VPS_ARGUMENT_ENUM_SOURCE_QUERY) {
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
        table->source_is_query = 1;
#endif
        if (!vps_build_query_source(table)) return SQLITE_ERROR;
    } else return SQLITE_ERROR;
    (void)error;
    return SQLITE_OK;
}

static int vps_table_describe(VpsTable *table, VpsError *error)
{
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    VpsQueryValidation validation;
    VpsClientStatus status;
    VpsQueryValidationResult validation_result;
    int validation_initialized = 0;
    int result = SQLITE_ERROR;
    (void)memset(&validation, 0, sizeof(validation));
    status = vps_client_connection_open(&table->client, &connection, error);
    if (status == VPS_CLIENT_OK)
        status = vps_client_connection_start(
            connection, VPS_CLIENT_OPERATION_CONNECT, error);
    if (status == VPS_CLIENT_OK) status = vps_drive_connection(connection, error);
    validation_result = status == VPS_CLIENT_OK
                            ? vps_query_validation_init(
                                  &validation, &table->allocator,
                                  (const char *)table->scan_query.data,
                                  table->scan_query.size, 10000U,
                                  &table->logger)
                            : VPS_QUERY_VALIDATION_CLIENT_ERROR;
    if (validation_result == VPS_QUERY_VALIDATION_OK) {
        validation_initialized = 1;
        status = vps_client_statement_open(
            connection, vps_query_validation_statement_spec(&validation),
            &statement, error);
    } else status = VPS_CLIENT_BACKEND_ERROR;
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_PREPARE, error);
    if (status == VPS_CLIENT_OK)
        status = vps_drive_statement(statement, NULL, error);
    if (status == VPS_CLIENT_OK &&
        vps_query_describe_result_init(&table->described,
                                       &table->allocator) ==
            VPS_QUERY_VALIDATION_OK) {
        table->initialized_described = 1;
        if (vps_query_validation_collect(statement, &table->described, error) ==
                VPS_QUERY_VALIDATION_OK &&
            table->described.field_count != 0U)
            result = SQLITE_OK;
    }
    if (statement != NULL) (void)vps_client_statement_close(&statement);
    if (connection != NULL) (void)vps_client_connection_close(&connection);
    if (validation_initialized) vps_query_validation_cleanup(&validation);
    return result;
}

static const char *vps_sqlite_declared_type(uint32_t type_oid)
{
    switch (vps_type_codec_for_oid(type_oid)) {
        case VPS_CODEC_BOOLEAN: case VPS_CODEC_INTEGER: return "INTEGER";
        case VPS_CODEC_FLOAT: return "REAL";
        case VPS_CODEC_BYTEA: return "BLOB";
        default: return "TEXT";
    }
}

static int vps_resolve_row_identity(VpsTable *table)
{
    const char *keys;
    size_t keys_length = 0U;
    size_t offset = 0U;
    keys = vps_argument_text(&table->arguments, VPS_ARGUMENT_ID_KEY_COLUMNS,
                             &keys_length);
    table->identity_mode = VPS_ROW_IDENTITY_SCAN_LOCAL;
    if (keys == NULL || keys_length == 0U) return 1;
    while (offset < keys_length) {
        size_t end = offset;
        size_t field;
        while (end < keys_length && keys[end] != ',') ++end;
        if (end == offset ||
            table->key_column_count >= VPS_QUERY_METADATA_MAX_KEY_COLUMNS)
            return 0;
        for (field = 0U; field < table->described.field_count; ++field) {
            const VpsQueryDescribeField *described =
                &table->described.fields[field];
            if (described->name_length == end - offset &&
                memcmp(described->name, keys + offset, end - offset) == 0)
                break;
        }
        if (field == table->described.field_count) return 0;
        table->key_columns[table->key_column_count++] = (uint16_t)field;
        offset = end + (end < keys_length ? 1U : 0U);
    }
    if (table->key_column_count == 1U &&
        vps_type_codec_for_oid(
            table->described.fields[table->key_columns[0]].type_oid) ==
            VPS_CODEC_INTEGER)
        table->identity_mode = VPS_ROW_IDENTITY_STABLE_INTEGER;
    else
        table->identity_mode = VPS_ROW_IDENTITY_HIDDEN_TOKEN;
    return 1;
}

static int vps_build_expected_fields(VpsTable *table)
{
    size_t index;
    if (vps_size_multiply(table->described.field_count,
                          sizeof(*table->expected_fields),
                          &table->expected_fields_size) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, table->expected_fields_size,
                            (void **)&table->expected_fields) != VPS_MEMORY_OK)
        return 0;
    for (index = 0U; index < table->described.field_count; ++index) {
        table->expected_fields[index].type_oid =
            table->described.fields[index].type_oid;
        table->expected_fields[index].format = VPS_CLIENT_VALUE_TEXT;
    }
    return 1;
}

static int vps_declare_schema(sqlite3 *database, VpsTable *table)
{
    VpsBuffer declaration;
    size_t index;
    int result;
    if (vps_buffer_init(&declaration, &table->allocator,
                        VPS_METADATA_MAX_TOTAL_BYTES) != VPS_MEMORY_OK ||
        !vps_append_bytes(&declaration, "CREATE TABLE x(", 15U))
        return SQLITE_NOMEM;
    for (index = 0U; index < table->described.field_count; ++index) {
        const VpsQueryDescribeField *field = &table->described.fields[index];
        const char *type = vps_sqlite_declared_type(field->type_oid);
        if ((index != 0U && !vps_append_bytes(&declaration, ",", 1U)) ||
            !vps_append_sqlite_identifier(&declaration, field->name,
                                          field->name_length) ||
            !vps_append_bytes(&declaration, " ", 1U) ||
            !vps_append_bytes(&declaration, type, strlen(type))) {
            vps_buffer_reset(&declaration);
            return SQLITE_NOMEM;
        }
    }
    if (table->identity_mode == VPS_ROW_IDENTITY_HIDDEN_TOKEN &&
        !vps_append_bytes(&declaration,
                          ",\"__vps_identity\" BLOB HIDDEN", 29U)) {
        vps_buffer_reset(&declaration);
        return SQLITE_NOMEM;
    }
    if (!vps_append_bytes(&declaration, ")\0", 2U)) {
        vps_buffer_reset(&declaration);
        return SQLITE_NOMEM;
    }
    result = sqlite3_declare_vtab(database, (const char *)declaration.data);
    vps_buffer_reset(&declaration);
    if (result != SQLITE_OK) return result;
    return sqlite3_vtab_config(database, SQLITE_VTAB_DIRECTONLY);
}

static uint64_t vps_vtab_source_fingerprint(const VpsTable *table)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    const unsigned char *query = table->scan_query.data;
    for (index = 0U; index < table->scan_query.size; ++index) {
        hash ^= query[index];
        hash *= UINT64_C(1099511628211);
    }
    for (index = 0U; index < table->described.field_count; ++index) {
        const VpsQueryDescribeField *field = &table->described.fields[index];
        size_t name_index;
        hash ^= field->type_oid;
        hash *= UINT64_C(1099511628211);
        hash ^= (uint32_t)field->type_modifier;
        hash *= UINT64_C(1099511628211);
        for (name_index = 0U; name_index < field->name_length; ++name_index) {
            hash ^= (unsigned char)field->name[name_index];
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static int vps_module_connect(sqlite3 *database,
                              void *auxiliary,
                              int argument_count,
                              const char *const *arguments,
                              sqlite3_vtab **table_out,
                              char **error_out)
{
    VpsTable *table;
    VpsError error;
    int error_initialized = 0;
    int result;
    if (database == NULL || table_out == NULL) return SQLITE_MISUSE;
    *table_out = NULL;
    table = (VpsTable *)sqlite3_malloc64(sizeof(*table));
    if (table == NULL) return SQLITE_NOMEM;
    (void)memset(table, 0, sizeof(*table));
    table->database = database;
    table->module_context = (VpsModuleContext *)auxiliary;
    if (table->module_context == NULL || table->module_context->closing) {
        sqlite3_free(table);
        return SQLITE_MISUSE;
    }
    table->table_id = ++table->module_context->next_table_id;
    table->module_context->table_references += 1U;
    result = vps_table_initialize_runtime(table, argument_count, arguments,
                                          &error);
    if (vps_error_init(&error, &table->allocator) == VPS_MEMORY_OK)
        error_initialized = 1;
    if (result == SQLITE_OK) result = vps_table_describe(table, &error);
    if (result == SQLITE_OK && !vps_build_expected_fields(table))
        result = SQLITE_NOMEM;
    if (result == SQLITE_OK && !vps_resolve_row_identity(table))
        result = SQLITE_ERROR;
    if (result == SQLITE_OK &&
        table->described.field_count > VPS_PLAN_MAX_COLUMNS)
        result = SQLITE_TOOBIG;
    if (result == SQLITE_OK)
        table->source_fingerprint = vps_vtab_source_fingerprint(table);
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    if (result == SQLITE_OK && !vps_table_materialization_init(table))
        result = SQLITE_ERROR;
#endif
    if (result == SQLITE_OK) result = vps_declare_schema(database, table);
    if (result != SQLITE_OK) {
        if (error_out != NULL)
            *error_out = sqlite3_mprintf(
                "%s", error_initialized && vps_error_message(&error) != NULL
                          ? vps_error_message(&error)
                          : "VirtualPostgreSQL connection or metadata validation failed");
        if (error_initialized) vps_error_reset(&error);
        (void)vps_table_cleanup(table);
        return result;
    }
    if (error_initialized) vps_error_reset(&error);
    *table_out = &table->base;
    return SQLITE_OK;
}

static int vps_module_disconnect(sqlite3_vtab *base)
{
    return vps_table_cleanup((VpsTable *)base);
}

static uint32_t vps_vtab_plan_capabilities(uint32_t type_oid)
{
    VpsCodecId codec = vps_type_codec_for_oid(type_oid);
    switch (codec) {
        case VPS_CODEC_BOOLEAN:
        case VPS_CODEC_INTEGER:
        case VPS_CODEC_UUID_TEXT:
            return VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_ORDER |
                   VPS_PLAN_CAP_EXACT;
        case VPS_CODEC_BYTEA:
            return VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_EXACT |
                   VPS_PLAN_CAP_BINARY;
        default:
            return 0U;
    }
}

static VpsPlanValueClass vps_vtab_value_class(int sqlite_type)
{
    switch (sqlite_type) {
        case SQLITE_NULL: return VPS_PLAN_VALUE_NULL;
        case SQLITE_INTEGER: return VPS_PLAN_VALUE_INTEGER;
        case SQLITE_FLOAT: return VPS_PLAN_VALUE_REAL;
        case SQLITE_TEXT: return VPS_PLAN_VALUE_TEXT;
        case SQLITE_BLOB: return VPS_PLAN_VALUE_BLOB;
        default: return VPS_PLAN_VALUE_UNKNOWN;
    }
}

static VpsPlanValueClass vps_vtab_expected_value_class(uint32_t type_oid)
{
    VpsCodecId codec = vps_type_codec_for_oid(type_oid);
    if (codec == VPS_CODEC_BYTEA) return VPS_PLAN_VALUE_BLOB;
    if (codec == VPS_CODEC_UUID_TEXT) return VPS_PLAN_VALUE_TEXT;
    if (codec == VPS_CODEC_BOOLEAN || codec == VPS_CODEC_INTEGER)
        return VPS_PLAN_VALUE_INTEGER;
    return VPS_PLAN_VALUE_UNKNOWN;
}

static int vps_vtab_constraint_operation(unsigned char sqlite_operation,
                                         VpsPlanOperator *operation,
                                         int *null_safe)
{
    *null_safe = 0;
    switch (sqlite_operation) {
        case SQLITE_INDEX_CONSTRAINT_EQ: *operation = VPS_PLAN_OP_EQ; return 1;
        case SQLITE_INDEX_CONSTRAINT_NE: *operation = VPS_PLAN_OP_NE; return 1;
        case SQLITE_INDEX_CONSTRAINT_LT: *operation = VPS_PLAN_OP_LT; return 1;
        case SQLITE_INDEX_CONSTRAINT_LE: *operation = VPS_PLAN_OP_LE; return 1;
        case SQLITE_INDEX_CONSTRAINT_GT: *operation = VPS_PLAN_OP_GT; return 1;
        case SQLITE_INDEX_CONSTRAINT_GE: *operation = VPS_PLAN_OP_GE; return 1;
        case SQLITE_INDEX_CONSTRAINT_ISNULL:
            *operation = VPS_PLAN_OP_IS_NULL; return 1;
        case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
            *operation = VPS_PLAN_OP_IS_NOT_NULL; return 1;
#if defined(SQLITE_INDEX_CONSTRAINT_IS)
        case SQLITE_INDEX_CONSTRAINT_IS:
            *operation = VPS_PLAN_OP_EQ; *null_safe = 1; return 1;
#endif
#if defined(SQLITE_INDEX_CONSTRAINT_LIMIT)
        case SQLITE_INDEX_CONSTRAINT_LIMIT:
            *operation = VPS_PLAN_OP_LIMIT; return 1;
#endif
#if defined(SQLITE_INDEX_CONSTRAINT_OFFSET)
        case SQLITE_INDEX_CONSTRAINT_OFFSET:
            *operation = VPS_PLAN_OP_OFFSET; return 1;
#endif
        default: return 0;
    }
}

static int vps_module_best_index(sqlite3_vtab *table,
                                 sqlite3_index_info *index_info)
{
    VpsTable *vtab = (VpsTable *)table;
    VpsPlannerColumn columns[VPS_PLAN_MAX_COLUMNS];
    VpsPlannerConstraintInput constraints[VPS_PLAN_MAX_CONSTRAINTS];
    VpsPlannerOrderInput order_terms[VPS_PLAN_MAX_ORDER_TERMS];
    VpsPlannerRequest request;
    VpsCompiledPlan plan;
    VpsBuffer encoded;
    size_t constraint_count = 0U;
    size_t order_count = 0U;
    size_t index;
    char *owned_plan;
    if (vtab == NULL || index_info == NULL ||
        vtab->described.field_count > VPS_PLAN_MAX_COLUMNS ||
        index_info->nConstraint < 0 ||
        (size_t)index_info->nConstraint > VPS_PLAN_MAX_CONSTRAINTS ||
        index_info->nOrderBy < 0 ||
        (size_t)index_info->nOrderBy > VPS_PLAN_MAX_ORDER_TERMS)
        return SQLITE_CONSTRAINT;
    (void)memset(columns, 0, sizeof(columns));
    (void)memset(constraints, 0, sizeof(constraints));
    (void)memset(order_terms, 0, sizeof(order_terms));
    for (index = 0U; index < vtab->described.field_count; ++index) {
        columns[index].type_oid = vtab->described.fields[index].type_oid;
        columns[index].capabilities =
            vps_vtab_plan_capabilities(columns[index].type_oid);
    }
    for (index = 0U; index < (size_t)index_info->nConstraint; ++index) {
        const struct sqlite3_index_constraint *source =
            &index_info->aConstraint[index];
        VpsPlannerConstraintInput *destination;
        VpsPlanOperator operation;
        sqlite3_value *rhs = NULL;
        int null_safe = 0;
        int is_in = 0;
        if (!vps_vtab_constraint_operation(source->op, &operation,
                                           &null_safe))
            continue;
        if (constraint_count >= VPS_PLAN_MAX_CONSTRAINTS) return SQLITE_TOOBIG;
        destination = &constraints[constraint_count++];
        destination->column = source->iColumn;
        destination->operation = operation;
        destination->source_index = (uint16_t)index;
        destination->usable = source->usable != 0;
        destination->null_safe = null_safe;
        if (source->usable && source->iColumn >= 0 &&
            operation == VPS_PLAN_OP_EQ &&
            (columns[source->iColumn].capabilities &
             (VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_EXACT)) ==
                (VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_EXACT) &&
            sqlite3_vtab_in(index_info, (int)index, -1)) {
            is_in = 1;
            destination->is_in = 1;
            (void)sqlite3_vtab_in(index_info, (int)index, 1);
        }
        if (operation == VPS_PLAN_OP_IS_NULL ||
            operation == VPS_PLAN_OP_IS_NOT_NULL ||
            operation == VPS_PLAN_OP_LIMIT ||
            operation == VPS_PLAN_OP_OFFSET)
            destination->value_class = VPS_PLAN_VALUE_INTEGER;
        else if (!is_in &&
                 sqlite3_vtab_rhs_value(index_info, (int)index, &rhs) ==
                     SQLITE_OK && rhs != NULL) {
            destination->value_class = vps_vtab_value_class(
                sqlite3_value_type(rhs));
            if (destination->value_class != VPS_PLAN_VALUE_NULL &&
                destination->value_class !=
                    vps_vtab_expected_value_class(
                        columns[source->iColumn].type_oid))
                destination->value_class = VPS_PLAN_VALUE_UNKNOWN;
            if (columns[source->iColumn].type_oid == 16U &&
                destination->value_class == VPS_PLAN_VALUE_INTEGER &&
                sqlite3_value_int64(rhs) != 0 && sqlite3_value_int64(rhs) != 1)
                destination->value_class = VPS_PLAN_VALUE_UNKNOWN;
            if (columns[source->iColumn].type_oid == 2950U &&
                destination->value_class == VPS_PLAN_VALUE_TEXT &&
                !vps_vtab_uuid_canonical(sqlite3_value_text(rhs),
                                          (size_t)sqlite3_value_bytes(rhs)))
                destination->value_class = VPS_PLAN_VALUE_UNKNOWN;
        }
        else if (is_in && source->iColumn >= 0) {
            destination->value_class =
                vps_vtab_expected_value_class(columns[source->iColumn].type_oid);
        }
    }
    for (index = 0U; index < (size_t)index_info->nOrderBy; ++index) {
        if (index_info->aOrderBy[index].iColumn < 0 ||
            (size_t)index_info->aOrderBy[index].iColumn >=
                vtab->described.field_count)
            continue;
        order_terms[order_count].column =
            (uint16_t)index_info->aOrderBy[index].iColumn;
        order_terms[order_count].descending =
            index_info->aOrderBy[index].desc != 0;
        ++order_count;
    }
    (void)memset(&request, 0, sizeof(request));
    request.source_fingerprint = vtab->source_fingerprint;
    request.columns = columns;
    request.column_count = vtab->described.field_count;
    request.columns_used = index_info->colUsed;
    request.constraints = constraints;
    request.constraint_count = constraint_count;
    request.order_terms = order_terms;
    request.order_count = order_count;
    request.key_columns = vtab->key_columns;
    request.key_column_count = vtab->key_column_count;
    request.estimated_base_rows = UINT64_C(1000000);
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    if (vtab->materialization != VPS_QUERY_MATERIALIZATION_OFF) {
        size_t best_prefix = 0U;
        size_t best_index = 0U;
        size_t query_index;
        for (query_index = 0U;
             query_index < vtab->query_indexes.index_count; ++query_index) {
            const VpsQueryIndexDefinition *definition =
                &vtab->query_indexes.indexes[query_index];
            size_t prefix = 0U;
            while (prefix < definition->column_count) {
                size_t constraint_index;
                int matched = 0;
                for (constraint_index = 0U;
                     constraint_index < constraint_count; ++constraint_index) {
                    const VpsPlannerConstraintInput *candidate =
                        &constraints[constraint_index];
                    if (candidate->usable && !candidate->is_in &&
                        candidate->column == definition->columns[prefix] &&
                        (candidate->operation == VPS_PLAN_OP_EQ ||
                         candidate->operation == VPS_PLAN_OP_IS_NULL)) {
                        matched = 1;
                        break;
                    }
                }
                if (!matched) break;
                prefix += 1U;
            }
            if (prefix > best_prefix) {
                best_prefix = prefix;
                best_index = query_index;
            }
        }
        request.query_index_prefix = best_prefix;
        request.query_index_ordinal = best_index;
    }
#endif
    request.logger = &vtab->logger;
    if (vps_planner_compile(&request, &plan) != VPS_PLANNER_OK ||
        vps_plan_encode(&plan, &vtab->allocator, &encoded) != VPS_PLANNER_OK)
        return SQLITE_ERROR;
    owned_plan = (char *)sqlite3_malloc64((sqlite3_uint64)encoded.size + 1U);
    if (owned_plan == NULL) {
        vps_buffer_reset(&encoded);
        return SQLITE_NOMEM;
    }
    (void)memcpy(owned_plan, encoded.data, encoded.size);
    owned_plan[encoded.size] = '\0';
    vps_buffer_reset(&encoded);
    for (index = 0U; index < plan.constraint_count; ++index) {
        const VpsPlanConstraint *constraint = &plan.constraints[index];
        struct sqlite3_index_constraint_usage *usage =
            &index_info->aConstraintUsage[constraint->source_index];
        usage->argvIndex = constraint->argument_index;
        usage->omit = (unsigned char)(
            (constraint->flags & VPS_PLAN_CONSTRAINT_RECHECK) == 0U);
    }
    index_info->idxNum = (int)VPS_PLAN_FORMAT_VERSION;
    index_info->idxStr = owned_plan;
    index_info->needToFreeIdxStr = 1;
    index_info->orderByConsumed =
        (plan.flags & VPS_PLAN_FLAG_ORDER_CONSUMED) != 0U;
    index_info->estimatedRows = (sqlite3_int64)plan.estimated_rows;
    index_info->estimatedCost = (double)plan.estimated_cost_milli / 1000.0;
    if ((plan.flags & VPS_PLAN_FLAG_UNIQUE) != 0U)
        index_info->idxFlags |= SQLITE_INDEX_SCAN_UNIQUE;
    return SQLITE_OK;
}

static int vps_module_open(sqlite3_vtab *base,
                           sqlite3_vtab_cursor **cursor_out)
{
    VpsTable *table = (VpsTable *)base;
    VpsCursor *cursor;
    if (table == NULL || cursor_out == NULL) return SQLITE_MISUSE;
    cursor = (VpsCursor *)sqlite3_malloc64(sizeof(*cursor));
    if (cursor == NULL) return SQLITE_NOMEM;
    (void)memset(cursor, 0, sizeof(*cursor));
    cursor->table = table;
    cursor->cursor_id = ++table->next_cursor_id;
    {
        VpsCursorLimits limits;
        vps_cursor_limits_default(&limits);
        if (vps_cursor_budget_init(&cursor->budget, &limits,
                                   cursor->cursor_id, &table->logger) !=
            VPS_CURSOR_LIMIT_OK) {
            sqlite3_free(cursor);
            return SQLITE_ERROR;
        }
    }
    if (vps_cursor_machine_init(&cursor->machine, cursor->cursor_id,
                                &table->logger) != VPS_CURSOR_OK ||
        vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_OPEN) !=
            VPS_CURSOR_OK ||
        vps_cancel_token_register(&table->module_context->cancel_registry,
                                  &cursor->cancel_token,
                                  cursor->cursor_id) !=
            VPS_CANCEL_REGISTRY_OK) {
        sqlite3_free(cursor);
        return SQLITE_ERROR;
    }
    table->active_cursors += 1U;
    *cursor_out = &cursor->base;
    return SQLITE_OK;
}

static void vps_cursor_row_reset(VpsCursor *cursor)
{
    size_t index;
    if (cursor == NULL) return;
    for (index = 0U; index < VPS_PLAN_MAX_COLUMNS; ++index)
        vps_decoded_value_reset(&cursor->decoded[index]);
    if (cursor->initialized_identity_token) {
        vps_buffer_reset(&cursor->identity_token);
        cursor->initialized_identity_token = 0;
    }
    cursor->row = NULL;
    cursor->row_bytes = 0U;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    cursor->snapshot_row = 0;
    (void)memset(cursor->snapshot_values, 0,
                 sizeof(cursor->snapshot_values));
#endif
}

static int vps_cursor_release(VpsCursor *cursor)
{
    int clean = 1;
    int drained = cursor->machine.state == VPS_CURSOR_EOF;
    VpsConnectionLeaseDisposition disposition;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    if (cursor->snapshot_scan != NULL) {
        if (vps_embedded_sqlite_scan_close(&cursor->snapshot_scan) !=
            VPS_EMBEDDED_SQLITE_OK) clean = 0;
        drained = 1;
    }
    if (cursor->snapshot_lease.owner != NULL &&
        vps_query_cache_lease_release(&cursor->snapshot_lease) !=
            VPS_QUERY_CACHE_OK) clean = 0;
#endif
    if (cursor->statement != NULL &&
        (cursor->machine.state == VPS_CURSOR_FILTERING ||
         cursor->machine.state == VPS_CURSOR_WAITING ||
         cursor->machine.state == VPS_CURSOR_ROW_READY)) {
        VpsError cancel_error;
        int error_initialized =
            vps_error_init(&cancel_error, &cursor->table->allocator) ==
            VPS_MEMORY_OK;
        if (!error_initialized ||
            vps_cursor_transition(&cursor->machine,
                                  VPS_CURSOR_EVENT_CANCEL) != VPS_CURSOR_OK ||
            vps_client_statement_start(cursor->statement,
                                       VPS_CLIENT_OPERATION_CANCEL,
                                       error_initialized ? &cancel_error
                                                         : NULL) !=
                VPS_CLIENT_OK ||
            vps_drive_statement(cursor->statement, &cursor->machine,
                                error_initialized ? &cancel_error : NULL) !=
                VPS_CLIENT_OK ||
            vps_cursor_transition(&cursor->machine,
                                  VPS_CURSOR_EVENT_COMPLETE) != VPS_CURSOR_OK) {
            clean = 0;
        } else {
            drained = 1;
        }
        if (error_initialized) vps_error_reset(&cancel_error);
    }
    disposition = drained ? VPS_CONNECTION_LEASE_CLEAN
                          : VPS_CONNECTION_LEASE_DIRTY;
    vps_cursor_row_reset(cursor);
    if (cursor->statement != NULL &&
        vps_client_statement_close(&cursor->statement) != VPS_CLIENT_OK)
        clean = 0;
    if (cursor->lease.pool != NULL &&
        vps_connection_lease_release(&cursor->lease,
                                     clean ? disposition
                                           : VPS_CONNECTION_LEASE_DIRTY) !=
            VPS_CONNECTION_POOL_OK)
        clean = 0;
    cursor->connection = NULL;
    if (cursor->initialized_planned_query) {
        vps_buffer_reset(&cursor->planned_query);
        cursor->initialized_planned_query = 0;
    }
    if (cursor->initialized_parameter_bytes) {
        vps_buffer_reset(&cursor->parameter_bytes);
        cursor->initialized_parameter_bytes = 0;
    }
    vps_memory_release(&cursor->table->allocator,
                       (void **)&cursor->parameters,
                       cursor->parameters_size);
    vps_memory_release(&cursor->table->allocator,
                       (void **)&cursor->projected_fields,
                       cursor->projected_fields_size);
    cursor->parameters_size = 0U;
    cursor->projected_fields_size = 0U;
    cursor->parameter_count = 0U;
    return clean ? SQLITE_OK : SQLITE_ERROR;
}

static int vps_module_close(sqlite3_vtab_cursor *base)
{
    VpsCursor *cursor = (VpsCursor *)base;
    VpsTable *table;
    int result;
    if (cursor == NULL) return SQLITE_OK;
    table = cursor->table;
    result = vps_cursor_release(cursor);
    if (vps_cancel_token_unregister(&cursor->cancel_token) !=
        VPS_CANCEL_REGISTRY_OK)
        result = SQLITE_ERROR;
    if (vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_CLOSE) !=
        VPS_CURSOR_OK)
        result = SQLITE_ERROR;
    if (table != NULL && table->active_cursors != 0U)
        table->active_cursors -= 1U;
    sqlite3_free(cursor);
    return result;
}

static int vps_cursor_advance(VpsCursor *cursor, VpsError *error)
{
    VpsClientStatus status;
    const VpsClientRowView *row = NULL;
    size_t index;
    size_t largest_column = 0U;
    if (cursor->row != NULL) {
        if (vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_FETCH) !=
            VPS_CURSOR_OK)
            goto failed;
        vps_cursor_row_reset(cursor);
        status = vps_client_statement_row_consumed(cursor->statement, error);
        if (status != VPS_CLIENT_OK) goto failed;
    }
    status = vps_client_statement_start(cursor->statement,
                                        VPS_CLIENT_OPERATION_FETCH, error);
    if (status == VPS_CLIENT_OK)
        status = vps_drive_statement(cursor->statement, &cursor->machine,
                                     error);
    if (status != VPS_CLIENT_OK) goto failed;
    if (cursor->machine.state == VPS_CURSOR_CANCELLING && error != NULL &&
        error->initialized &&
        vps_client_statement_state(cursor->statement) ==
            VPS_CLIENT_STATEMENT_COMPLETE)
        (void)vps_error_set_sqlstate(error, VPS_ERROR_OPERATION_CANCEL,
                                     "57014", 0, 0, NULL);
    if (error != NULL && error->initialized &&
        error->sqlite_code == SQLITE_INTERRUPT)
        goto failed;
    if (vps_client_statement_state(cursor->statement) ==
        VPS_CLIENT_STATEMENT_COMPLETE) {
        if (vps_cursor_transition(&cursor->machine,
                                  VPS_CURSOR_EVENT_COMPLETE) != VPS_CURSOR_OK)
            goto failed;
        return SQLITE_OK;
    }
    status = vps_client_statement_current_row(cursor->statement, &row, error);
    if (status != VPS_CLIENT_OK ||
        vps_client_row_column_count(row) != cursor->plan.projection_count)
        goto failed;
    cursor->row_bytes = 0U;
    for (index = 0U; index < vps_client_row_column_count(row); ++index) {
        VpsClientColumnView column;
        size_t logical;
        VpsCodecId codec;
        if (vps_client_row_column(row, index, &column, error) != VPS_CLIENT_OK ||
            column.length > SIZE_MAX - cursor->row_bytes)
            goto failed;
        cursor->row_bytes += column.length;
        if (column.length > largest_column) largest_column = column.length;
        if (index >= cursor->plan.projection_count) goto failed;
        logical = cursor->plan.projection[index];
        if (logical >= VPS_PLAN_MAX_COLUMNS ||
            cursor->decoded[logical].initialized)
            goto failed;
        codec = vps_type_codec_for_oid(column.type_oid);
        if (vps_type_codec_decode(&cursor->table->allocator, codec, &column,
                                  &cursor->decoded[logical]) !=
            VPS_TYPE_CODEC_OK)
            goto failed;
    }
    if (cursor->table->identity_mode == VPS_ROW_IDENTITY_STABLE_INTEGER) {
        VpsClientColumnView key;
        if (cursor->logical_to_remote[cursor->table->key_columns[0]] ==
                UINT16_MAX ||
            vps_client_row_column(
                row,
                cursor->logical_to_remote[cursor->table->key_columns[0]],
                &key, error) !=
                VPS_CLIENT_OK ||
            vps_row_identity_stable_integer(&key, &cursor->rowid) !=
                VPS_ROW_IDENTITY_OK)
            goto failed;
    } else if (cursor->table->identity_mode ==
               VPS_ROW_IDENTITY_HIDDEN_TOKEN) {
        VpsClientColumnView keys[VPS_QUERY_METADATA_MAX_KEY_COLUMNS];
        size_t key_index;
        (void)memset(keys, 0, sizeof(keys));
        for (key_index = 0U;
             key_index < cursor->table->key_column_count; ++key_index) {
            size_t logical = cursor->table->key_columns[key_index];
            if (logical >= VPS_PLAN_MAX_COLUMNS ||
                cursor->logical_to_remote[logical] == UINT16_MAX ||
                vps_client_row_column(
                    row, cursor->logical_to_remote[logical], &keys[key_index],
                    error) != VPS_CLIENT_OK)
                goto failed;
        }
        if (vps_row_identity_token(&cursor->table->allocator, keys,
                                   cursor->table->key_column_count,
                                   &cursor->identity_token) !=
            VPS_ROW_IDENTITY_OK)
            goto failed;
        cursor->initialized_identity_token = 1;
        if (vps_row_identity_scan_next(&cursor->scan_counter,
                                       &cursor->rowid) !=
            VPS_ROW_IDENTITY_OK)
            goto failed;
    } else if (vps_row_identity_scan_next(&cursor->scan_counter,
                                          &cursor->rowid) !=
               VPS_ROW_IDENTITY_OK) goto failed;
    if (vps_cursor_budget_observe_row(
            &cursor->budget, (uint64_t)cursor->row_bytes,
            (uint64_t)largest_column,
            cursor->initialized_identity_token
                ? (uint64_t)cursor->identity_token.size
                : 0U) != VPS_CURSOR_LIMIT_OK) {
        if (error != NULL && error->initialized) {
            (void)vps_error_set_local(error, VPS_ERROR_OPERATION_SCAN,
                                      VPS_ERROR_CLASS_MEMORY, NULL);
            error->sqlite_code = SQLITE_TOOBIG;
        }
        goto failed;
    }
    cursor->row = row;
    if (vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_ROW) !=
        VPS_CURSOR_OK)
        goto failed;
    return SQLITE_OK;
failed:
    vps_cursor_row_reset(cursor);
    (void)vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_FAIL);
    return vps_vtab_set_error(&cursor->table->base,
                              error != NULL && error->initialized &&
                                      error->sqlite_code != SQLITE_OK
                                  ? error->sqlite_code
                                  : SQLITE_ERROR,
                              error,
                              "VirtualPostgreSQL scan failed");
}

static int vps_vtab_append_placeholder(VpsBuffer *query, size_t number)
{
    char buffer[32];
    int length = snprintf(buffer, sizeof(buffer), "$%llu",
                          (unsigned long long)number);
    return length > 0 && (size_t)length < sizeof(buffer) &&
           vps_append_bytes(query, buffer, (size_t)length);
}

static int vps_vtab_uuid_canonical(const unsigned char *value, size_t length)
{
    size_t index;
    if (value == NULL || length != 36U) return 0;
    for (index = 0U; index < length; ++index) {
        int separator = index == 8U || index == 13U || index == 18U ||
                        index == 23U;
        if (separator) {
            if (value[index] != '-') return 0;
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'a' && value[index] <= 'f')))
            return 0;
    }
    return 1;
}

static int vps_vtab_value_compatible(sqlite3_value *value,
                                     uint32_t type_oid,
                                     VpsPlanValueClass expected)
{
    int type;
    if (value == NULL) return 0;
    type = sqlite3_value_type(value);
    if (type == SQLITE_NULL) return 1;
    if (expected == VPS_PLAN_VALUE_BLOB) return type == SQLITE_BLOB;
    if (type_oid == 16U)
        return type == SQLITE_INTEGER &&
               (sqlite3_value_int64(value) == 0 ||
                sqlite3_value_int64(value) == 1);
    if (expected == VPS_PLAN_VALUE_INTEGER) return type == SQLITE_INTEGER;
    if (type_oid == 2950U && type == SQLITE_TEXT)
        return vps_vtab_uuid_canonical(sqlite3_value_text(value),
                                       (size_t)sqlite3_value_bytes(value));
    return expected == VPS_PLAN_VALUE_TEXT && type == SQLITE_TEXT;
}

static int vps_vtab_add_parameter(VpsCursor *cursor,
                                  size_t *offsets,
                                  sqlite3_value *value,
                                  uint32_t type_oid,
                                  VpsPlanValueClass expected)
{
    VpsClientParameterView *parameter;
    const void *bytes = NULL;
    size_t length = 0U;
    size_t storage_length;
    char integer[32];
    int integer_length = 0;
    int sqlite_type;
    if (cursor->parameter_count >= VPS_VTAB_PARAMETER_LIMIT ||
        !vps_vtab_value_compatible(value, type_oid, expected)) return 0;
    sqlite_type = sqlite3_value_type(value);
    parameter = &cursor->parameters[cursor->parameter_count];
    (void)memset(parameter, 0, sizeof(*parameter));
    parameter->type_oid = type_oid;
    parameter->format = expected == VPS_PLAN_VALUE_BLOB
                            ? VPS_CLIENT_VALUE_BINARY
                            : VPS_CLIENT_VALUE_TEXT;
    parameter->is_null = sqlite_type == SQLITE_NULL;
    offsets[cursor->parameter_count] = cursor->parameter_bytes.size;
    if (!parameter->is_null) {
        if (expected == VPS_PLAN_VALUE_BLOB) {
            bytes = sqlite3_value_blob(value);
            length = (size_t)sqlite3_value_bytes(value);
        } else if (type_oid == 16U) {
            bytes = sqlite3_value_int64(value) == 0 ? "false" : "true";
            length = sqlite3_value_int64(value) == 0 ? 5U : 4U;
        } else if (expected == VPS_PLAN_VALUE_INTEGER) {
            integer_length = snprintf(integer, sizeof(integer), "%lld",
                                      (long long)sqlite3_value_int64(value));
            if (integer_length <= 0 || (size_t)integer_length >= sizeof(integer))
                return 0;
            bytes = integer;
            length = (size_t)integer_length;
        } else {
            bytes = sqlite3_value_text(value);
            length = (size_t)sqlite3_value_bytes(value);
        }
        storage_length = length;
        if (expected != VPS_PLAN_VALUE_BLOB) ++storage_length;
        else if (storage_length == 0U) storage_length = 1U;
        if (length == 0U) {
            static const unsigned char empty = 0U;
            bytes = &empty;
        }
        if (cursor->parameter_bytes.size > VPS_VTAB_PARAMETER_BYTES_LIMIT -
                                               storage_length ||
            vps_buffer_append(&cursor->parameter_bytes, bytes, length) !=
                VPS_MEMORY_OK ||
            (expected != VPS_PLAN_VALUE_BLOB &&
             vps_buffer_append(&cursor->parameter_bytes, "\0", 1U) !=
                 VPS_MEMORY_OK) ||
            (expected == VPS_PLAN_VALUE_BLOB && length == 0U &&
             vps_buffer_append(&cursor->parameter_bytes, bytes, 1U) !=
                 VPS_MEMORY_OK))
            return 0;
        parameter->length = length;
    }
    ++cursor->parameter_count;
    return 1;
}

static const char *vps_vtab_operator_sql(const VpsPlanConstraint *constraint)
{
    if ((constraint->flags & VPS_PLAN_CONSTRAINT_NULL_SAFE) != 0U)
        return " IS NOT DISTINCT FROM ";
    switch ((VpsPlanOperator)constraint->operation) {
        case VPS_PLAN_OP_EQ: return " = ";
        case VPS_PLAN_OP_NE: return " <> ";
        case VPS_PLAN_OP_LT: return " < ";
        case VPS_PLAN_OP_LE: return " <= ";
        case VPS_PLAN_OP_GT: return " > ";
        case VPS_PLAN_OP_GE: return " >= ";
        default: return NULL;
    }
}

static int vps_vtab_append_column_name(VpsBuffer *query,
                                       const VpsTable *table,
                                       size_t column)
{
    const VpsQueryDescribeField *field;
    if (column >= table->described.field_count) return 0;
    field = &table->described.fields[column];
    return vps_append_pg_identifier(query, field->name, field->name_length);
}

static int vps_vtab_in_compatible(sqlite3_value *list,
                                  const VpsPlanConstraint *constraint,
                                  size_t *item_count)
{
    sqlite3_value *item = NULL;
    int result;
    *item_count = 0U;
    if (list == NULL) return 0;
    result = sqlite3_vtab_in_first(list, &item);
    while (result == SQLITE_OK && item != NULL) {
        if (*item_count >= VPS_PLAN_DEFAULT_IN_LIMIT ||
            !vps_vtab_value_compatible(
                item, constraint->type_oid,
                (VpsPlanValueClass)constraint->value_class))
            return 0;
        ++*item_count;
        result = sqlite3_vtab_in_next(list, &item);
    }
    return result == SQLITE_DONE || (result == SQLITE_OK && item == NULL);
}

static int vps_vtab_build_execution(VpsCursor *cursor,
                                    int argument_count,
                                    sqlite3_value **arguments)
{
    VpsTable *table = cursor->table;
    size_t *offsets = NULL;
    size_t offsets_size = 0U;
    size_t index;
    size_t where_count = 0U;
    int result = 0;
    if (cursor->plan.projection_count == 0U ||
        cursor->plan.projection_count > table->described.field_count ||
        cursor->plan.argument_count != (uint16_t)argument_count ||
        vps_size_multiply(VPS_VTAB_PARAMETER_LIMIT, sizeof(*cursor->parameters),
                          &cursor->parameters_size) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, cursor->parameters_size,
                            (void **)&cursor->parameters) != VPS_MEMORY_OK ||
        vps_size_multiply(VPS_VTAB_PARAMETER_LIMIT, sizeof(*offsets),
                          &offsets_size) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, offsets_size,
                            (void **)&offsets) != VPS_MEMORY_OK ||
        vps_size_multiply(cursor->plan.projection_count,
                          sizeof(*cursor->projected_fields),
                          &cursor->projected_fields_size) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, cursor->projected_fields_size,
                            (void **)&cursor->projected_fields) != VPS_MEMORY_OK ||
        vps_buffer_init(&cursor->planned_query, &table->allocator,
                        VPS_VTAB_QUERY_LIMIT) != VPS_MEMORY_OK)
        goto cleanup;
    cursor->initialized_planned_query = 1;
    if (vps_buffer_init(&cursor->parameter_bytes, &table->allocator,
                        VPS_VTAB_PARAMETER_BYTES_LIMIT) != VPS_MEMORY_OK)
        goto cleanup;
    cursor->initialized_parameter_bytes = 1;
    for (index = 0U; index < VPS_PLAN_MAX_COLUMNS; ++index)
        cursor->logical_to_remote[index] = UINT16_MAX;
    if (!vps_append_bytes(&cursor->planned_query, "SELECT ", 7U)) goto cleanup;
    for (index = 0U; index < cursor->plan.projection_count; ++index) {
        size_t logical = cursor->plan.projection[index];
        if (logical >= table->described.field_count ||
            (index != 0U &&
             !vps_append_bytes(&cursor->planned_query, ",", 1U)) ||
            !vps_vtab_append_column_name(&cursor->planned_query, table, logical))
            goto cleanup;
        cursor->logical_to_remote[logical] = (uint16_t)index;
        cursor->projected_fields[index] = table->expected_fields[logical];
    }
    if (!vps_append_bytes(&cursor->planned_query, " FROM (", 7U) ||
        !vps_append_bytes(&cursor->planned_query,
                          (const char *)table->scan_query.data,
                          table->scan_query.size) ||
        !vps_append_bytes(&cursor->planned_query, ") AS vps_source", 15U))
        goto cleanup;
    for (index = 0U; index < cursor->plan.constraint_count; ++index) {
        const VpsPlanConstraint *constraint = &cursor->plan.constraints[index];
        sqlite3_value *argument = constraint->argument_index == 0U
                                      ? NULL
                                      : arguments[constraint->argument_index - 1U];
        size_t item_count = 0U;
        size_t item_index = 0U;
        sqlite3_value *item = NULL;
        int in_result;
        const char *operator_sql;
        const char *where_prefix;
        if (constraint->operation == VPS_PLAN_OP_LIMIT ||
            constraint->operation == VPS_PLAN_OP_OFFSET) continue;
        if ((constraint->flags & VPS_PLAN_CONSTRAINT_IN) != 0U &&
            !vps_vtab_in_compatible(argument, constraint, &item_count))
            continue;
        if ((constraint->flags & VPS_PLAN_CONSTRAINT_IN) == 0U &&
            constraint->argument_index != 0U &&
            !vps_vtab_value_compatible(
                argument, constraint->type_oid,
                (VpsPlanValueClass)constraint->value_class))
            continue;
        where_prefix = where_count == 0U ? " WHERE " : " AND ";
        if (!vps_append_bytes(&cursor->planned_query, where_prefix,
                              strlen(where_prefix)))
            goto cleanup;
        ++where_count;
        if ((constraint->flags & VPS_PLAN_CONSTRAINT_IN) != 0U &&
            item_count == 0U) {
            if (!vps_append_bytes(&cursor->planned_query, "FALSE", 5U))
                goto cleanup;
            continue;
        }
        if (!vps_vtab_append_column_name(&cursor->planned_query, table,
                                         (size_t)constraint->column))
            goto cleanup;
        if (constraint->operation == VPS_PLAN_OP_IS_NULL) {
            if (!vps_append_bytes(&cursor->planned_query, " IS NULL", 8U))
                goto cleanup;
            continue;
        }
        if (constraint->operation == VPS_PLAN_OP_IS_NOT_NULL) {
            if (!vps_append_bytes(&cursor->planned_query, " IS NOT NULL", 12U))
                goto cleanup;
            continue;
        }
        if ((constraint->flags & VPS_PLAN_CONSTRAINT_IN) != 0U) {
            if (!vps_append_bytes(&cursor->planned_query, " IN (", 5U))
                goto cleanup;
            in_result = sqlite3_vtab_in_first(argument, &item);
            while (in_result == SQLITE_OK && item != NULL) {
                if ((item_index != 0U &&
                     !vps_append_bytes(&cursor->planned_query, ",", 1U)) ||
                    !vps_vtab_add_parameter(
                        cursor, offsets, item, constraint->type_oid,
                        (VpsPlanValueClass)constraint->value_class) ||
                    !vps_vtab_append_placeholder(&cursor->planned_query,
                                                 cursor->parameter_count))
                    goto cleanup;
                ++item_index;
                in_result = sqlite3_vtab_in_next(argument, &item);
            }
            if (!(in_result == SQLITE_DONE ||
                  (in_result == SQLITE_OK && item == NULL)) ||
                !vps_append_bytes(&cursor->planned_query, ")", 1U))
                goto cleanup;
            continue;
        }
        operator_sql = vps_vtab_operator_sql(constraint);
        if (operator_sql == NULL ||
            !vps_append_bytes(&cursor->planned_query, operator_sql,
                              strlen(operator_sql)) ||
            !vps_vtab_add_parameter(
                cursor, offsets, argument, constraint->type_oid,
                (VpsPlanValueClass)constraint->value_class) ||
            !vps_vtab_append_placeholder(&cursor->planned_query,
                                         cursor->parameter_count))
            goto cleanup;
    }
    if ((cursor->plan.flags & VPS_PLAN_FLAG_ORDER_CONSUMED) != 0U) {
        if (!vps_append_bytes(&cursor->planned_query, " ORDER BY ", 10U))
            goto cleanup;
        for (index = 0U; index < cursor->plan.order_count; ++index) {
            const VpsPlanOrderTerm *term = &cursor->plan.order_terms[index];
            const char *suffix = term->descending
                                     ? " DESC NULLS LAST" : " ASC NULLS FIRST";
            if ((index != 0U &&
                 !vps_append_bytes(&cursor->planned_query, ",", 1U)) ||
                !vps_vtab_append_column_name(&cursor->planned_query, table,
                                             term->column) ||
                !vps_append_bytes(&cursor->planned_query, suffix,
                                  strlen(suffix)))
                goto cleanup;
        }
    }
    for (index = 0U; index < cursor->plan.constraint_count; ++index) {
        const VpsPlanConstraint *constraint = &cursor->plan.constraints[index];
        sqlite3_value *argument;
        sqlite3_int64 value;
        const char *keyword;
        if (constraint->operation != VPS_PLAN_OP_LIMIT &&
            constraint->operation != VPS_PLAN_OP_OFFSET) continue;
        argument = arguments[constraint->argument_index - 1U];
        if (sqlite3_value_type(argument) != SQLITE_INTEGER) goto cleanup;
        value = sqlite3_value_int64(argument);
        if (constraint->operation == VPS_PLAN_OP_LIMIT && value < 0) continue;
        keyword = constraint->operation == VPS_PLAN_OP_LIMIT
                      ? " LIMIT " : " OFFSET ";
        if (!vps_append_bytes(&cursor->planned_query, keyword,
                              strlen(keyword)))
            goto cleanup;
        if (constraint->operation == VPS_PLAN_OP_OFFSET && value < 0) {
            if (!vps_append_bytes(&cursor->planned_query, "0", 1U))
                goto cleanup;
        } else if (!vps_vtab_add_parameter(
                       cursor, offsets, argument, 20U,
                       VPS_PLAN_VALUE_INTEGER) ||
                   !vps_vtab_append_placeholder(&cursor->planned_query,
                                                cursor->parameter_count))
            goto cleanup;
    }
    for (index = 0U; index < cursor->parameter_count; ++index)
        if (!cursor->parameters[index].is_null)
            cursor->parameters[index].value =
                cursor->parameter_bytes.data + offsets[index];
    if (!vps_append_bytes(&cursor->planned_query, "\0", 1U)) goto cleanup;
    cursor->planned_query.size -= 1U;
    result = 1;
cleanup:
    vps_memory_release(&table->allocator, (void **)&offsets, offsets_size);
    return result;
}

#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
static int vps_snapshot_value_from_host(sqlite3_value *source,
                                        VpsEmbeddedValue *value)
{
    int type;
    if (source == NULL || value == NULL) return 0;
    (void)memset(value, 0, sizeof(*value));
    type = sqlite3_value_type(source);
    switch (type) {
        case SQLITE_NULL: value->kind = VPS_EMBEDDED_VALUE_NULL; break;
        case SQLITE_INTEGER:
            value->kind = VPS_EMBEDDED_VALUE_INTEGER;
            value->integer = (int64_t)sqlite3_value_int64(source);
            break;
        case SQLITE_FLOAT:
            value->kind = VPS_EMBEDDED_VALUE_REAL;
            value->real = sqlite3_value_double(source);
            break;
        case SQLITE_TEXT:
            value->kind = VPS_EMBEDDED_VALUE_TEXT;
            value->bytes = sqlite3_value_text(source);
            value->length = (size_t)sqlite3_value_bytes(source);
            break;
        case SQLITE_BLOB:
            value->kind = VPS_EMBEDDED_VALUE_BLOB;
            value->bytes = sqlite3_value_blob(source);
            value->length = (size_t)sqlite3_value_bytes(source);
            break;
        default: return 0;
    }
    return 1;
}

static VpsEmbeddedOperator vps_snapshot_operator(uint8_t operation)
{
    switch ((VpsPlanOperator)operation) {
        case VPS_PLAN_OP_EQ: return VPS_EMBEDDED_OP_EQ;
        case VPS_PLAN_OP_NE: return VPS_EMBEDDED_OP_NE;
        case VPS_PLAN_OP_LT: return VPS_EMBEDDED_OP_LT;
        case VPS_PLAN_OP_LE: return VPS_EMBEDDED_OP_LE;
        case VPS_PLAN_OP_GT: return VPS_EMBEDDED_OP_GT;
        case VPS_PLAN_OP_GE: return VPS_EMBEDDED_OP_GE;
        case VPS_PLAN_OP_IS_NULL: return VPS_EMBEDDED_OP_IS_NULL;
        case VPS_PLAN_OP_IS_NOT_NULL: return VPS_EMBEDDED_OP_IS_NOT_NULL;
        default: return 0;
    }
}

static int vps_snapshot_advance(VpsCursor *cursor)
{
    int has_row = 0;
    size_t index;
    size_t row_bytes = 0U;
    size_t largest_column = 0U;
    if (cursor->snapshot_row) {
        if (vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_FETCH) !=
            VPS_CURSOR_OK) return SQLITE_ERROR;
        vps_cursor_row_reset(cursor);
    }
    if (vps_embedded_sqlite_scan_step(cursor->snapshot_scan, &has_row) !=
        VPS_EMBEDDED_SQLITE_OK) goto failed;
    if (!has_row) {
        if (vps_cursor_transition(&cursor->machine,
                                  VPS_CURSOR_EVENT_COMPLETE) != VPS_CURSOR_OK)
            goto failed;
        return SQLITE_OK;
    }
    for (index = 0U; index < cursor->plan.projection_count; ++index) {
        size_t logical = cursor->plan.projection[index];
        VpsEmbeddedValue *value = &cursor->snapshot_values[logical];
        if (vps_embedded_sqlite_scan_column(cursor->snapshot_scan, index,
                                            value) != VPS_EMBEDDED_SQLITE_OK)
            goto failed;
        if (value->length > SIZE_MAX - row_bytes) goto failed;
        row_bytes += value->length;
        if (value->length > largest_column) largest_column = value->length;
    }
    if (cursor->table->identity_mode == VPS_ROW_IDENTITY_STABLE_INTEGER) {
        const VpsEmbeddedValue *key =
            &cursor->snapshot_values[cursor->table->key_columns[0]];
        if (key->kind != VPS_EMBEDDED_VALUE_INTEGER) goto failed;
        cursor->rowid = key->integer;
    } else {
        if (vps_row_identity_scan_next(&cursor->scan_counter,
                                       &cursor->rowid) != VPS_ROW_IDENTITY_OK)
            goto failed;
        if (cursor->table->identity_mode == VPS_ROW_IDENTITY_HIDDEN_TOKEN) {
            VpsClientColumnView keys[VPS_QUERY_METADATA_MAX_KEY_COLUMNS];
            char integer_text[VPS_QUERY_METADATA_MAX_KEY_COLUMNS][32];
            size_t key_index;
            (void)memset(keys, 0, sizeof(keys));
            for (key_index = 0U;
                 key_index < cursor->table->key_column_count; ++key_index) {
                const VpsEmbeddedValue *key = &cursor->snapshot_values[
                    cursor->table->key_columns[key_index]];
                keys[key_index].is_null = key->kind == VPS_EMBEDDED_VALUE_NULL;
                if (key->kind == VPS_EMBEDDED_VALUE_INTEGER) {
                    int length = snprintf(integer_text[key_index],
                                          sizeof(integer_text[key_index]),
                                          "%lld", (long long)key->integer);
                    if (length <= 0 ||
                        (size_t)length >= sizeof(integer_text[key_index]))
                        goto failed;
                    keys[key_index].data = integer_text[key_index];
                    keys[key_index].length = (size_t)length;
                } else {
                    keys[key_index].data = key->bytes;
                    keys[key_index].length = key->length;
                }
            }
            if (vps_row_identity_token(
                    &cursor->table->allocator, keys,
                    cursor->table->key_column_count,
                    &cursor->identity_token) != VPS_ROW_IDENTITY_OK)
                goto failed;
            cursor->initialized_identity_token = 1;
        }
    }
    if (vps_cursor_budget_observe_row(
            &cursor->budget, (uint64_t)row_bytes,
            (uint64_t)largest_column,
            cursor->initialized_identity_token
                ? (uint64_t)cursor->identity_token.size : 0U) !=
        VPS_CURSOR_LIMIT_OK) goto failed;
    cursor->row_bytes = row_bytes;
    cursor->snapshot_row = 1;
    if (vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_ROW) !=
        VPS_CURSOR_OK) goto failed;
    return SQLITE_OK;
failed:
    vps_cursor_row_reset(cursor);
    (void)vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_FAIL);
    return vps_vtab_set_error(&cursor->table->base, SQLITE_ERROR, NULL,
                              "VirtualPostgreSQL snapshot scan failed");
}

static int vps_snapshot_filter(VpsCursor *cursor,
                               int argument_count,
                               sqlite3_value **arguments)
{
    VpsEmbeddedConstraint constraints[VPS_EMBEDDED_MAX_CONSTRAINTS];
    VpsEmbeddedOrderTerm order[VPS_EMBEDDED_MAX_ORDER_TERMS];
    VpsEmbeddedScanRequest request;
    size_t constraint_count = 0U;
    size_t index;
    if (cursor->plan.argument_count != (uint16_t)argument_count)
        return SQLITE_CORRUPT_VTAB;
    (void)memset(constraints, 0, sizeof(constraints));
    (void)memset(order, 0, sizeof(order));
    (void)memset(&request, 0, sizeof(request));
    for (index = 0U; index < cursor->plan.constraint_count; ++index) {
        const VpsPlanConstraint *source = &cursor->plan.constraints[index];
        if (source->operation == VPS_PLAN_OP_LIMIT ||
            source->operation == VPS_PLAN_OP_OFFSET) {
            sqlite3_int64 value = sqlite3_value_int64(
                arguments[source->argument_index - 1U]);
            if (source->operation == VPS_PLAN_OP_LIMIT) {
                if (value >= 0) {
                    request.has_limit = 1;
                    request.limit = (uint64_t)value;
                }
            } else {
                request.has_offset = 1;
                request.offset = value < 0 ? 0U : (uint64_t)value;
            }
            continue;
        }
        if ((source->flags & VPS_PLAN_CONSTRAINT_IN) != 0U) continue;
        constraints[constraint_count].column = (uint16_t)source->column;
        constraints[constraint_count].operation =
            vps_snapshot_operator(source->operation);
        if (constraints[constraint_count].operation == 0) continue;
        if (source->argument_index != 0U &&
            !vps_snapshot_value_from_host(
                arguments[source->argument_index - 1U],
                &constraints[constraint_count].value))
            return SQLITE_MISMATCH;
        constraint_count += 1U;
    }
    for (index = 0U; index < cursor->plan.order_count; ++index) {
        order[index].column = cursor->plan.order_terms[index].column;
        order[index].descending = cursor->plan.order_terms[index].descending;
    }
    if (vps_query_cache_acquire(
            cursor->table->query_cache, 60000U, vps_table_build_snapshot,
            cursor->table, &cursor->snapshot_lease) != VPS_QUERY_CACHE_OK)
        return vps_vtab_set_error(&cursor->table->base, SQLITE_ERROR, NULL,
                                  "VirtualPostgreSQL snapshot build failed");
    request.projection = cursor->plan.projection;
    request.projection_count = cursor->plan.projection_count;
    request.constraints = constraints;
    request.constraint_count = constraint_count;
    request.order_terms = order;
    request.order_count = cursor->plan.order_count;
    request.use_index = cursor->plan.selected_index_prefix != 0U;
    request.selected_index = cursor->plan.selected_index_ordinal;
    if (vps_embedded_sqlite_scan_open(
            cursor->snapshot_lease.database, &request,
            &cursor->snapshot_scan) != VPS_EMBEDDED_SQLITE_OK)
        return vps_vtab_set_error(&cursor->table->base, SQLITE_ERROR, NULL,
                                  "VirtualPostgreSQL snapshot plan failed");
    return vps_snapshot_advance(cursor);
}
#endif

static int vps_module_filter(sqlite3_vtab_cursor *base,
                             int index_number,
                             const char *index_string,
                             int argument_count,
                             sqlite3_value **arguments)
{
    VpsCursor *cursor = (VpsCursor *)base;
    VpsTable *table;
    VpsClientStatementSpec spec;
    VpsError error;
    VpsClientStatus status;
    int error_initialized = 0;
    int result;
    size_t index_string_length;
    if (cursor == NULL || index_number != (int)VPS_PLAN_FORMAT_VERSION ||
        index_string == NULL || argument_count < 0 ||
        (cursor->machine.state != VPS_CURSOR_OPEN &&
         cursor->machine.state != VPS_CURSOR_EOF))
        return SQLITE_MISUSE;
    table = cursor->table;
    (void)vps_cursor_release(cursor);
    if (vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_FILTER) !=
        VPS_CURSOR_OK)
        return SQLITE_MISUSE;
    cursor->scan_counter = 0U;
    vps_cursor_budget_reset(&cursor->budget);
    index_string_length = strlen(index_string);
    if (vps_plan_decode(index_string, index_string_length,
                        table->source_fingerprint, &cursor->plan) !=
            VPS_PLANNER_OK) {
        (void)vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_FAIL);
        return vps_vtab_set_error(&table->base, SQLITE_CORRUPT_VTAB, NULL,
                                  "VirtualPostgreSQL compiled plan rejected");
    }
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    if (table->materialization != VPS_QUERY_MATERIALIZATION_OFF)
        return vps_snapshot_filter(cursor, argument_count, arguments);
#endif
    if (!vps_vtab_build_execution(cursor, argument_count, arguments)) {
        (void)vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_FAIL);
        return vps_vtab_set_error(&table->base, SQLITE_CORRUPT_VTAB, NULL,
                                  "VirtualPostgreSQL compiled plan rejected");
    }
    if (vps_cursor_budget_check_query(
            &cursor->budget, (uint64_t)cursor->planned_query.size,
            (uint64_t)cursor->parameter_bytes.size,
            0U) != VPS_CURSOR_LIMIT_OK) {
        (void)vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_FAIL);
        return vps_vtab_set_error(&table->base, SQLITE_TOOBIG, NULL,
                                  "VirtualPostgreSQL scan limit exceeded");
    }
    if (vps_error_init(&error, &table->allocator) == VPS_MEMORY_OK)
        error_initialized = 1;
    status = vps_connection_pool_acquire(
                 table->pool, &table->pool_key, 10000U, NULL, NULL,
                 &cursor->lease) == VPS_CONNECTION_POOL_OK
                 ? VPS_CLIENT_OK
                 : VPS_CLIENT_BACKEND_ERROR;
    if (status == VPS_CLIENT_OK)
        cursor->connection =
            (VpsClientConnection *)cursor->lease.connection;
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = (const char *)cursor->planned_query.data;
    spec.query_length = cursor->planned_query.size;
    spec.parameters = cursor->parameters;
    spec.parameter_count = cursor->parameter_count;
    spec.result_fields = cursor->projected_fields;
    spec.result_field_count = cursor->plan.projection_count;
    spec.timeout_ms = 60000U;
    spec.single_row = 1;
    spec.interrupt_probe = vps_cursor_interrupt_probe;
    spec.interrupt_context = cursor;
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_open(cursor->connection, &spec,
                                           &cursor->statement, &error);
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_start(cursor->statement,
                                            VPS_CLIENT_OPERATION_EXECUTE,
                                            &error);
    if (status == VPS_CLIENT_OK)
        status = vps_drive_statement(cursor->statement, &cursor->machine,
                                     &error);
    result = status == VPS_CLIENT_OK
                 ? vps_cursor_advance(cursor, &error)
                 : vps_vtab_set_error(
                       &table->base,
                       error_initialized && error.sqlite_code != SQLITE_OK
                           ? error.sqlite_code
                           : SQLITE_ERROR,
                       &error, "VirtualPostgreSQL filter failed");
    if (status != VPS_CLIENT_OK)
        (void)vps_cursor_transition(&cursor->machine, VPS_CURSOR_EVENT_FAIL);
    if (error_initialized) vps_error_reset(&error);
    return result;
}

static int vps_module_next(sqlite3_vtab_cursor *base)
{
    VpsCursor *cursor = (VpsCursor *)base;
    VpsError error;
    int result;
    if (cursor == NULL || cursor->machine.state != VPS_CURSOR_ROW_READY)
        return SQLITE_MISUSE;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    if (cursor->snapshot_scan != NULL)
        return vps_snapshot_advance(cursor);
#endif
    if (vps_error_init(&error, &cursor->table->allocator) != VPS_MEMORY_OK)
        return SQLITE_NOMEM;
    result = vps_cursor_advance(cursor, &error);
    vps_error_reset(&error);
    return result;
}

static int vps_module_eof(sqlite3_vtab_cursor *base)
{
    const VpsCursor *cursor = (const VpsCursor *)base;
    return cursor == NULL || cursor->machine.state == VPS_CURSOR_EOF;
}

static int vps_module_column(sqlite3_vtab_cursor *base,
                             sqlite3_context *context,
                             int column_index)
{
    VpsCursor *cursor = (VpsCursor *)base;
    const VpsDecodedValue *decoded;
    if (cursor == NULL || context == NULL ||
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
        (cursor->row == NULL && !cursor->snapshot_row) ||
#else
        cursor->row == NULL ||
#endif
        column_index < 0 ||
        (size_t)column_index > cursor->table->described.field_count)
        return SQLITE_MISUSE;
    if ((size_t)column_index == cursor->table->described.field_count) {
        if (cursor->table->identity_mode != VPS_ROW_IDENTITY_HIDDEN_TOKEN)
            return SQLITE_MISUSE;
        if (!cursor->initialized_identity_token) return SQLITE_CORRUPT_VTAB;
        sqlite3_result_blob64(context, cursor->identity_token.data,
                              (sqlite3_uint64)cursor->identity_token.size,
                              SQLITE_TRANSIENT);
        return SQLITE_OK;
    }
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    if (cursor->snapshot_row) {
        const VpsEmbeddedValue *value;
        size_t projection;
        int projected = 0;
        for (projection = 0U;
             projection < cursor->plan.projection_count; ++projection)
            if (cursor->plan.projection[projection] ==
                (uint16_t)column_index) projected = 1;
        if (!projected) return SQLITE_CORRUPT_VTAB;
        value = &cursor->snapshot_values[column_index];
        switch (value->kind) {
            case VPS_EMBEDDED_VALUE_NULL: sqlite3_result_null(context); break;
            case VPS_EMBEDDED_VALUE_INTEGER:
                sqlite3_result_int64(context, (sqlite3_int64)value->integer);
                break;
            case VPS_EMBEDDED_VALUE_REAL:
                sqlite3_result_double(context, value->real);
                break;
            case VPS_EMBEDDED_VALUE_TEXT:
                sqlite3_result_text64(context, (const char *)value->bytes,
                                      (sqlite3_uint64)value->length,
                                      SQLITE_TRANSIENT, SQLITE_UTF8);
                break;
            case VPS_EMBEDDED_VALUE_BLOB:
                sqlite3_result_blob64(context, value->bytes,
                                      (sqlite3_uint64)value->length,
                                      SQLITE_TRANSIENT);
                break;
            default: return SQLITE_CORRUPT_VTAB;
        }
        return SQLITE_OK;
    }
#endif
    if ((size_t)column_index >= VPS_PLAN_MAX_COLUMNS ||
        cursor->logical_to_remote[column_index] == UINT16_MAX ||
        !cursor->decoded[column_index].initialized)
        return SQLITE_CORRUPT_VTAB;
    decoded = &cursor->decoded[column_index];
    switch (decoded->kind) {
        case VPS_DECODED_NULL: sqlite3_result_null(context); break;
        case VPS_DECODED_INTEGER:
            sqlite3_result_int64(context, (sqlite3_int64)decoded->integer);
            break;
        case VPS_DECODED_REAL:
            sqlite3_result_double(context, decoded->real);
            break;
        case VPS_DECODED_TEXT:
            sqlite3_result_text64(context, (const char *)decoded->bytes,
                                  (sqlite3_uint64)decoded->length,
                                  SQLITE_TRANSIENT, SQLITE_UTF8);
            break;
        case VPS_DECODED_BLOB:
            sqlite3_result_blob64(context, decoded->bytes,
                                  (sqlite3_uint64)decoded->length,
                                  SQLITE_TRANSIENT);
            break;
        default: sqlite3_result_error(context, "invalid codec state", -1); break;
    }
    return SQLITE_OK;
}

static int vps_module_rowid(sqlite3_vtab_cursor *base,
                            sqlite3_int64 *rowid_out)
{
    const VpsCursor *cursor = (const VpsCursor *)base;
    if (cursor == NULL || rowid_out == NULL ||
        cursor->machine.state != VPS_CURSOR_ROW_READY) return SQLITE_MISUSE;
    *rowid_out = (sqlite3_int64)cursor->rowid;
    return SQLITE_OK;
}

static int vps_module_shadow_name(const char *name)
{
    size_t length;
    if (name == NULL) return 0;
    length = strlen(name);
    return (length >= 11U && strcmp(name + length - 11U, "_vps_schema") == 0) ||
           (length >= 13U && strcmp(name + length - 13U, "_vps_metadata") == 0);
}

static int vps_module_integrity(sqlite3_vtab *table,
                                const char *schema,
                                const char *table_name,
                                int flags,
                                char **error_out)
{
    (void)table; (void)schema; (void)table_name; (void)flags;
    if (error_out != NULL) *error_out = NULL;
    return SQLITE_OK;
}

const sqlite3_module VPS_MODULE = {
    .iVersion = 4,
    .xCreate = vps_module_connect,
    .xConnect = vps_module_connect,
    .xBestIndex = vps_module_best_index,
    .xDisconnect = vps_module_disconnect,
    .xDestroy = vps_module_disconnect,
    .xOpen = vps_module_open,
    .xClose = vps_module_close,
    .xFilter = vps_module_filter,
    .xNext = vps_module_next,
    .xEof = vps_module_eof,
    .xColumn = vps_module_column,
    .xRowid = vps_module_rowid,
    .xShadowName = vps_module_shadow_name,
    .xIntegrity = vps_module_integrity
};

static int vps_metadata_connect(sqlite3 *database,
                                void *auxiliary,
                                int argument_count,
                                const char *const *arguments,
                                sqlite3_vtab **table_out,
                                char **error_out)
{
    VpsEmptyTable *table;
    int result;
    (void)auxiliary; (void)arguments; (void)error_out;
    if (database == NULL || table_out == NULL || argument_count != 3)
        return SQLITE_MISUSE;
    result = sqlite3_declare_vtab(
        database, "CREATE TABLE x(name TEXT, connection HIDDEN)");
    if (result != SQLITE_OK) return result;
    result = sqlite3_vtab_config(database, SQLITE_VTAB_DIRECTONLY);
    if (result != SQLITE_OK) return result;
    table = (VpsEmptyTable *)sqlite3_malloc64(sizeof(*table));
    if (table == NULL) return SQLITE_NOMEM;
    (void)memset(table, 0, sizeof(*table));
    *table_out = &table->base;
    return SQLITE_OK;
}

static int vps_metadata_disconnect(sqlite3_vtab *table)
{
    sqlite3_free(table);
    return SQLITE_OK;
}

static int vps_metadata_open(sqlite3_vtab *table,
                             sqlite3_vtab_cursor **cursor_out)
{
    VpsEmptyCursor *cursor;
    (void)table;
    if (cursor_out == NULL) return SQLITE_MISUSE;
    cursor = (VpsEmptyCursor *)sqlite3_malloc64(sizeof(*cursor));
    if (cursor == NULL) return SQLITE_NOMEM;
    (void)memset(cursor, 0, sizeof(*cursor));
    cursor->is_eof = 1;
    *cursor_out = &cursor->base;
    return SQLITE_OK;
}

static int vps_metadata_close(sqlite3_vtab_cursor *cursor)
{
    sqlite3_free(cursor);
    return SQLITE_OK;
}

static int vps_metadata_filter(sqlite3_vtab_cursor *cursor,
                               int index_number,
                               const char *index_string,
                               int argument_count,
                               sqlite3_value **arguments)
{
    (void)index_number; (void)index_string; (void)argument_count;
    (void)arguments;
    if (cursor == NULL) return SQLITE_MISUSE;
    ((VpsEmptyCursor *)cursor)->is_eof = 1;
    return SQLITE_OK;
}

static int vps_metadata_best_index(sqlite3_vtab *table,
                                   sqlite3_index_info *index_info)
{
    (void)table;
    if (index_info == NULL) return SQLITE_MISUSE;
    index_info->estimatedCost = 1000000.0;
    index_info->estimatedRows = 1000000;
    return SQLITE_OK;
}

static int vps_metadata_next(sqlite3_vtab_cursor *cursor)
{
    if (cursor == NULL) return SQLITE_MISUSE;
    ((VpsEmptyCursor *)cursor)->is_eof = 1;
    return SQLITE_OK;
}

static int vps_metadata_eof(sqlite3_vtab_cursor *cursor)
{
    return cursor == NULL || ((VpsEmptyCursor *)cursor)->is_eof;
}

static int vps_metadata_column(sqlite3_vtab_cursor *cursor,
                               sqlite3_context *context,
                               int column)
{
    (void)cursor; (void)column;
    sqlite3_result_null(context);
    return SQLITE_OK;
}

static int vps_metadata_rowid(sqlite3_vtab_cursor *cursor,
                              sqlite3_int64 *rowid)
{
    (void)cursor;
    if (rowid == NULL) return SQLITE_MISUSE;
    *rowid = 0;
    return SQLITE_OK;
}

const sqlite3_module VPS_METADATA_MODULE = {
    .iVersion = 4,
    .xConnect = vps_metadata_connect,
    .xBestIndex = vps_metadata_best_index,
    .xDisconnect = vps_metadata_disconnect,
    .xOpen = vps_metadata_open,
    .xClose = vps_metadata_close,
    .xFilter = vps_metadata_filter,
    .xNext = vps_metadata_next,
    .xEof = vps_metadata_eof,
    .xColumn = vps_metadata_column,
    .xRowid = vps_metadata_rowid,
    .xIntegrity = vps_module_integrity
};
