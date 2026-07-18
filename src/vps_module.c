#include "vps_module.h"
SQLITE_EXTENSION_INIT3

#include "vps_arguments.h"
#include "vps_cancel.h"
#include "vps_connection_string.h"
#include "vps_connection_pool.h"
#include "vps_cursor.h"
#include "vps_dml.h"
#include "vps_identity.h"
#include "vps_libpq_client.h"
#include "vps_libpq_client_conninfo.h"
#include "vps_metadata_cache.h"
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
#include "vps_spatial.h"
#include "vps_spatial_client.h"
#include "vps_table_metadata.h"
#include "vps_tls_policy.h"
#include "vps_transaction.h"
#include "vps_type_codec.h"
#if defined(_WIN32)
#include "vps_wincred_provider.h"
#endif

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
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
    VpsTableMetadata table_metadata;
    VpsDmlPolicy dml_policy;
    VpsSpatialCapabilities spatial;
    VpsSpatialFormat spatial_format;
    unsigned char spatial_kind[VPS_PLAN_MAX_COLUMNS];
    VpsSpatialGeometryType spatial_type[VPS_PLAN_MAX_COLUMNS];
    uint32_t spatial_dimensions[VPS_PLAN_MAX_COLUMNS];
    uint32_t spatial_srid[VPS_PLAN_MAX_COLUMNS];
    VpsClientResultFieldExpectation *expected_fields;
    size_t expected_fields_size;
    VpsBuffer scan_query;
    uint64_t source_fingerprint;
    VpsModuleContext *module_context;
    uint16_t key_columns[VPS_QUERY_METADATA_MAX_KEY_COLUMNS];
    size_t key_column_count;
    VpsRowIdentityMode identity_mode;
    VpsDmlOptimisticMode optimistic_mode;
    VpsArgumentEnumValue transaction_isolation;
    size_t visible_field_count;
    int mode_rw;
    int transaction_read_only;
    int source_is_query;
    int has_hidden_identity;
    int initialized_table_metadata;
    int initialized_dml_policy;
    int initialized_spatial;
    uint64_t table_id;
    uint64_t next_cursor_id;
    size_t active_cursors;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    VpsQueryIndexSet query_indexes;
    VpsQueryCache *query_cache;
    VpsTempFilePath temp_path;
    VpsQueryMaterializationMode materialization;
#endif
    int initialized_arguments;
#if defined(_WIN32)
#endif
    int initialized_config;
    int initialized_identity;
    int initialized_adapter;
    int initialized_client;
    int initialized_pool;
    int initialized_described;
    int initialized_query;
    char *shadow_schema;
    char *shadow_name;
    int metadata_cached;
    int schema_refresh;
    int cache_fallback;
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
    size_t remote_projection_count;
    size_t optimistic_remote_column;
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
    int transaction_stream;
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    VpsQueryCacheLease snapshot_lease;
    VpsEmbeddedSqliteScan *snapshot_scan;
    VpsEmbeddedValue snapshot_values[VPS_PLAN_MAX_COLUMNS];
    int snapshot_row;
#endif
} VpsCursor;

typedef enum VpsMetadataFunctionKind {
    VPS_METADATA_FUNCTION_RELATIONS = 0,
    VPS_METADATA_FUNCTION_TABLE_INFO = 1,
    VPS_METADATA_FUNCTION_INDEX_LIST = 2,
    VPS_METADATA_FUNCTION_INDEX_INFO = 3,
    VPS_METADATA_FUNCTION_TYPE_INFO = 4,
    VPS_METADATA_FUNCTION_EXTENSIONS = 5
} VpsMetadataFunctionKind;

typedef struct VpsMetadataTable {
    sqlite3_vtab base;
    sqlite3 *database;
    VpsModuleContext *module_context;
    VpsMetadataFunctionKind kind;
    VpsCatalogQuery query;
    size_t visible_count;
    size_t hidden_count;
} VpsMetadataTable;

typedef struct VpsMetadataCursor {
    sqlite3_vtab_cursor base;
    VpsMetadataTable *table;
    VpsTable *runtime;
    VpsConnectionLease lease;
    VpsMetadataRowSet rows;
    size_t row;
    int initialized_rows;
} VpsMetadataCursor;

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
    if (vps_allocator_system(&context->transaction_allocator) != VPS_MEMORY_OK ||
        vps_transaction_init(&context->transaction,
                             &context->transaction_allocator, NULL) !=
            VPS_TRANSACTION_OK) {
        sqlite3_free(context);
        return NULL;
    }
    context->initialized_transaction = 1;
    if (vps_cancel_registry_init(&context->cancel_registry,
                                 vps_platform_current_operations(), NULL) !=
        VPS_CANCEL_REGISTRY_OK) {
        vps_transaction_cleanup(&context->transaction);
        sqlite3_free(context);
        return NULL;
    }
    context->initialized_cancel_registry = 1;
#if defined(_WIN32)
    {
        VpsCredentialProvider provider;
        if (vps_allocator_system(&context->credential_allocator) !=
                VPS_MEMORY_OK ||
            vps_credential_registry_init(
                &context->credential_registry,
                vps_platform_current_operations(), NULL) !=
                VPS_CREDENTIAL_REGISTRY_OK) {
            (void)vps_cancel_registry_cleanup(&context->cancel_registry);
            vps_transaction_cleanup(&context->transaction);
            sqlite3_free(context);
            return NULL;
        }
        context->initialized_credential_registry = 1;
        if (vps_wincred_provider_init(
                &context->wincred, &context->credential_allocator,
                vps_platform_current_operations(), NULL) !=
                VPS_CREDENTIAL_REGISTRY_OK) {
            (void)vps_credential_registry_cleanup(
                &context->credential_registry);
            (void)vps_cancel_registry_cleanup(&context->cancel_registry);
            vps_transaction_cleanup(&context->transaction);
            sqlite3_free(context);
            return NULL;
        }
        context->initialized_wincred = 1;
        if (vps_wincred_provider_make(&context->wincred, &provider) !=
                VPS_CREDENTIAL_REGISTRY_OK ||
            vps_credential_registry_register(
                &context->credential_registry, UINT64_C(1), &provider) !=
                VPS_CREDENTIAL_REGISTRY_OK) {
            (void)vps_wincred_provider_cleanup(&context->wincred);
            (void)vps_credential_registry_cleanup(
                &context->credential_registry);
            (void)vps_cancel_registry_cleanup(&context->cancel_registry);
            vps_transaction_cleanup(&context->transaction);
            sqlite3_free(context);
            return NULL;
        }
    }
#endif
    return context;
}

void vps_module_context_destroy(void *opaque)
{
    VpsModuleContext *context = (VpsModuleContext *)opaque;
    if (context == NULL) return;
    context->closing = 1;
    if (context->transaction_lease.pool != NULL)
        (void)vps_connection_lease_release(&context->transaction_lease,
                                           VPS_CONNECTION_LEASE_DIRTY);
    context->transaction_connection = NULL;
    if (context->initialized_transaction)
        vps_transaction_cleanup(&context->transaction);
#if defined(_WIN32)
    if (context->initialized_credential_registry)
        (void)vps_credential_registry_cleanup(
            &context->credential_registry);
    if (context->initialized_wincred)
        (void)vps_wincred_provider_cleanup(&context->wincred);
#endif
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
        !vps_append_bytes(&table->scan_query, "SELECT vps_source.*", 19U) ||
        (table->optimistic_mode == VPS_DML_OPTIMISTIC_XMIN &&
         !vps_append_bytes(&table->scan_query,
                           ",xmin::pg_catalog.text AS \"__vps_xmin\"",
                           38U)) ||
        !vps_append_bytes(&table->scan_query, " FROM ", 6U) ||
        !vps_append_pg_identifier(&table->scan_query, schema, schema_length) ||
        !vps_append_bytes(&table->scan_query, ".", 1U) ||
        !vps_append_pg_identifier(&table->scan_query, relation,
                                  relation_length) ||
        !vps_append_bytes(&table->scan_query, " AS vps_source", 14U) ||
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
    VpsEmbeddedValueKind *kinds = NULL;
    VpsEmbeddedIndexDefinition *indexes = NULL;
    size_t kinds_bytes = sizeof(*kinds) * VPS_PLAN_MAX_COLUMNS;
    size_t indexes_bytes = sizeof(*indexes) * VPS_QUERY_INDEX_MAX_COUNT;
    VpsEmbeddedSchema schema;
    VpsConnectionLease lease;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    VpsClientStatementSpec spec;
    VpsCursorLimits limits;
    VpsCursorBudget budget;
    VpsError error;
    VpsDecodedValue *decoded = NULL;
    VpsEmbeddedValue *values = NULL;
    size_t decoded_bytes = sizeof(*decoded) * VPS_PLAN_MAX_COLUMNS;
    size_t values_bytes = sizeof(*values) * VPS_PLAN_MAX_COLUMNS;
    VpsClientStatus client_status = VPS_CLIENT_BACKEND_ERROR;
    VpsQueryCacheStatus result = VPS_QUERY_CACHE_BUILD_FAILED;
    int error_initialized = 0;
    int complete = 0;
    size_t index;
    (void)memset(&lease, 0, sizeof(lease));
    if (vps_memory_allocate(&table->allocator, kinds_bytes,
                            (void **)&kinds) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, indexes_bytes,
                            (void **)&indexes) != VPS_MEMORY_OK) {
        vps_memory_release(&table->allocator, (void **)&indexes,
                           indexes_bytes);
        vps_memory_release(&table->allocator, (void **)&kinds, kinds_bytes);
        return VPS_QUERY_CACHE_BUILD_FAILED;
    }
    (void)memset(indexes, 0, indexes_bytes);
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
    client_status = vps_embedded_sqlite_create_schema(snapshot, &schema) ==
                            VPS_EMBEDDED_SQLITE_OK
                        ? VPS_CLIENT_OK
                        : VPS_CLIENT_BACKEND_ERROR;
    vps_memory_release(&table->allocator, (void **)&indexes, indexes_bytes);
    vps_memory_release(&table->allocator, (void **)&kinds, kinds_bytes);
    if (client_status != VPS_CLIENT_OK) return VPS_QUERY_CACHE_BUILD_FAILED;
    if (vps_memory_allocate(&table->allocator, decoded_bytes,
                            (void **)&decoded) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, values_bytes,
                            (void **)&values) != VPS_MEMORY_OK) {
        vps_memory_release(&table->allocator, (void **)&values, values_bytes);
        vps_memory_release(&table->allocator, (void **)&decoded,
                           decoded_bytes);
        return VPS_QUERY_CACHE_BUILD_FAILED;
    }
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
        uint64_t row_bytes = 0U;
        uint64_t largest_column = 0U;
        size_t column;
        (void)memset(decoded, 0, decoded_bytes);
        (void)memset(values, 0, values_bytes);
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
    vps_memory_release(&table->allocator, (void **)&values, values_bytes);
    vps_memory_release(&table->allocator, (void **)&decoded, decoded_bytes);
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
    if (table->initialized_table_metadata)
        vps_table_metadata_reset(&table->table_metadata);
    if (table->initialized_spatial)
        vps_spatial_capabilities_reset(&table->spatial);
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
    if (table->initialized_arguments &&
        vps_arguments_reset(&table->arguments) != VPS_ARGUMENTS_OK) clean = 0;
    sqlite3_free(table->shadow_schema);
    sqlite3_free(table->shadow_name);
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
    const VpsArgumentValue *mode;
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
    mode = vps_arguments_get(&table->arguments, VPS_ARGUMENT_ID_MODE);
    table->mode_rw = mode != NULL && mode->present &&
                     mode->enum_value == VPS_ARGUMENT_ENUM_MODE_RW;
    {
        const VpsArgumentValue *metadata_mode = vps_arguments_get(
            &table->arguments, VPS_ARGUMENT_ID_METADATA_MODE);
        const VpsArgumentValue *schema_policy = vps_arguments_get(
            &table->arguments, VPS_ARGUMENT_ID_SCHEMA_POLICY);
        table->metadata_cached = metadata_mode != NULL &&
            metadata_mode->present && metadata_mode->enum_value ==
                VPS_ARGUMENT_ENUM_METADATA_CACHED;
        table->schema_refresh = schema_policy != NULL &&
            schema_policy->present && schema_policy->enum_value ==
                VPS_ARGUMENT_ENUM_SCHEMA_REFRESH;
    }
    {
        const VpsArgumentValue *isolation = vps_arguments_get(
            &table->arguments, VPS_ARGUMENT_ID_ISOLATION);
        const VpsArgumentValue *read_only = vps_arguments_get(
            &table->arguments, VPS_ARGUMENT_ID_TRANSACTION_READ_ONLY);
        table->transaction_isolation =
            isolation != NULL && isolation->present
                ? isolation->enum_value
                : VPS_ARGUMENT_ENUM_ISOLATION_READ_COMMITTED;
        table->transaction_read_only =
            read_only != NULL && read_only->present &&
            read_only->boolean_value;
    }
    table->optimistic_mode = VPS_DML_OPTIMISTIC_OFF;
    if (table->mode_rw) {
        const char *optimistic;
        size_t optimistic_length = 0U;
        optimistic = vps_argument_text(&table->arguments,
                                       VPS_ARGUMENT_ID_OPTIMISTIC_LOCK,
                                       &optimistic_length);
        if (optimistic != NULL && optimistic_length != 0U &&
            !(optimistic_length == 3U &&
              memcmp(optimistic, "off", 3U) == 0)) {
            if (optimistic_length == 6U &&
                memcmp(optimistic, "column", 6U) == 0)
                table->optimistic_mode = VPS_DML_OPTIMISTIC_COLUMN;
            else if (optimistic_length == 4U &&
                     memcmp(optimistic, "xmin", 4U) == 0)
                table->optimistic_mode = VPS_DML_OPTIMISTIC_XMIN;
            else
                return SQLITE_ERROR;
        }
    }
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
    if (table->module_context == NULL ||
        !table->module_context->initialized_credential_registry)
        return SQLITE_ERROR;
    resolve_options.credential_registry =
        &table->module_context->credential_registry;
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
    table->pool_key.read_only = table->mode_rw ? 0 : 1;
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
        table->source_is_query = 1;
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
    const char *schema = NULL;
    const char *relation = NULL;
    size_t schema_length = 0U;
    size_t relation_length = 0U;
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
    if (result == SQLITE_OK && !table->source_is_query) {
        schema = vps_argument_text(&table->arguments, VPS_ARGUMENT_ID_SCHEMA,
                                   &schema_length);
        relation = vps_argument_text(&table->arguments, VPS_ARGUMENT_ID_TABLE,
                                     &relation_length);
        if (schema == NULL || relation == NULL ||
            vps_table_metadata_load(
                &table->table_metadata, connection, &table->allocator,
                schema, schema_length, relation, relation_length,
                &table->logger, error) != VPS_METADATA_OK)
            result = SQLITE_ERROR;
        else
            table->initialized_table_metadata = 1;
    }
    if (result == SQLITE_OK) {
        VpsSpatialResult spatial_result = vps_spatial_capabilities_load(
            &table->spatial, connection, &table->allocator, &table->logger,
            error);
        if (spatial_result == VPS_SPATIAL_OK ||
            spatial_result == VPS_SPATIAL_NOT_AVAILABLE)
            table->initialized_spatial = 1;
        else
            result = SQLITE_ERROR;
    }
    if (connection != NULL) (void)vps_client_connection_close(&connection);
    if (validation_initialized) vps_query_validation_cleanup(&validation);
    return result;
}

static int vps_table_build_dml_policy(VpsTable *table)
{
    const char *mode;
    const char *version_column;
    size_t mode_length = 0U;
    size_t version_length = 0U;
    VpsDmlResult result;
    if (!table->mode_rw) return 1;
    if (table->source_is_query || !table->initialized_table_metadata)
        return 0;
    mode = vps_argument_text(&table->arguments,
                             VPS_ARGUMENT_ID_OPTIMISTIC_LOCK, &mode_length);
    version_column = vps_argument_text(&table->arguments,
                                       VPS_ARGUMENT_ID_VERSION_COLUMN,
                                       &version_length);
    (void)mode;
    (void)mode_length;
    result = vps_dml_policy_build(
        &table->table_metadata, 1, table->optimistic_mode,
        version_column, version_length, &table->dml_policy);
    if (result != VPS_DML_OK) return 0;
    table->dml_policy.spatial = &table->spatial;
    table->dml_policy.spatial_format = table->spatial_format;
    if (table->dml_policy.visible_count > VPS_PLAN_MAX_COLUMNS) return 0;
    (void)memcpy(table->dml_policy.spatial_kind, table->spatial_kind,
                 table->dml_policy.visible_count);
    (void)memcpy(table->dml_policy.spatial_srid, table->spatial_srid,
                 table->dml_policy.visible_count * sizeof(uint32_t));
    {
        size_t visible;
        for (visible = 0U; visible < table->dml_policy.visible_count;
             ++visible) {
            const VpsColumnMetadata *column;
            if (table->dml_policy.spatial_kind[visible] == 0U ||
                table->spatial_format == VPS_SPATIAL_FORMAT_NONE)
                continue;
            column = &table->table_metadata.columns.columns[
                table->dml_policy.visible_to_metadata[visible]];
            table->dml_policy.selections[visible].capabilities |=
                VPS_CODEC_CAP_DML;
            if (column->generated_kind == '\0') {
                table->dml_policy.updatable[visible] =
                    column->identity_kind == '\0';
                table->dml_policy.insertable[visible] =
                    column->identity_kind != 'a';
            }
        }
    }
    table->initialized_dml_policy = 1;
    return 1;
}

static VpsSpatialFormat vps_table_spatial_format(const VpsTable *table)
{
    const VpsArgumentValue *value = vps_arguments_get(
        &table->arguments, VPS_ARGUMENT_ID_GEOMETRY);
    if (value == NULL || !value->present) return VPS_SPATIAL_FORMAT_WKT;
    switch (value->enum_value) {
        case VPS_ARGUMENT_ENUM_GEOMETRY_WKB: return VPS_SPATIAL_FORMAT_WKB;
        case VPS_ARGUMENT_ENUM_GEOMETRY_EWKT: return VPS_SPATIAL_FORMAT_EWKT;
        case VPS_ARGUMENT_ENUM_GEOMETRY_EWKB: return VPS_SPATIAL_FORMAT_EWKB;
        case VPS_ARGUMENT_ENUM_GEOMETRY_SPATIALITE:
            return VPS_SPATIAL_FORMAT_SPATIALITE;
        case VPS_ARGUMENT_ENUM_GEOMETRY_NONE: return VPS_SPATIAL_FORMAT_NONE;
        case VPS_ARGUMENT_ENUM_GEOMETRY_WKT:
        default: return VPS_SPATIAL_FORMAT_WKT;
    }
}

static int vps_table_integrate_spatial(VpsTable *table)
{
    const VpsArgumentValue *srid_argument;
    VpsBuffer projected;
    uint32_t override_srid = 0U;
    size_t index;
    int has_spatial = 0;
    if (table == NULL || !table->initialized_spatial ||
        table->described.field_count > VPS_PLAN_MAX_COLUMNS)
        return 0;
    table->spatial_format = vps_table_spatial_format(table);
    if (table->spatial_format == VPS_SPATIAL_FORMAT_SPATIALITE)
        return 0;
    srid_argument = vps_arguments_get(&table->arguments, VPS_ARGUMENT_ID_SRID);
    if (srid_argument != NULL && srid_argument->present)
        override_srid = srid_argument->uint32_value;
    if (override_srid > INT32_MAX) return 0;
    for (index = 0U; index < table->described.field_count; ++index) {
        VpsQueryDescribeField *field = &table->described.fields[index];
        VpsSpatialGeometryType typmod_type = VPS_SPATIAL_TYPE_ANY;
        uint32_t typmod_dimensions = 0U;
        uint32_t typmod_srid = 0U;
        if (table->spatial.present &&
            field->type_oid == table->spatial.geometry_oid)
            table->spatial_kind[index] = 1U;
        else if (table->spatial.present &&
                 field->type_oid == table->spatial.geography_oid)
            table->spatial_kind[index] = 2U;
        else
            continue;
        if (vps_spatial_typmod_decode(field->type_modifier, &typmod_type,
                                      &typmod_dimensions, &typmod_srid) !=
            VPS_SPATIAL_OK)
            return 0;
        if (override_srid != 0U && typmod_srid != 0U &&
            override_srid != typmod_srid)
            return 0;
        table->spatial_type[index] = typmod_type;
        table->spatial_dimensions[index] = typmod_dimensions;
        table->spatial_srid[index] = override_srid != 0U
                                         ? override_srid : typmod_srid;
        has_spatial = 1;
    }
    if (!has_spatial || table->spatial_format == VPS_SPATIAL_FORMAT_NONE)
        return 1;
    if (vps_buffer_init(&projected, &table->allocator,
                        VPS_VTAB_QUERY_LIMIT) != VPS_MEMORY_OK)
        return 0;
    if (!vps_append_bytes(&projected, "SELECT ", 7U)) goto fail;
    for (index = 0U; index < table->described.field_count; ++index) {
        VpsQueryDescribeField *field = &table->described.fields[index];
        if (index != 0U && !vps_append_bytes(&projected, ",", 1U)) goto fail;
        if (table->spatial_kind[index] != 0U) {
            VpsSpatialExpression expression;
            VpsSpatialKind kind = table->spatial_kind[index] == 1U
                                      ? VPS_SPATIAL_KIND_GEOMETRY
                                      : VPS_SPATIAL_KIND_GEOGRAPHY;
            if (vps_spatial_read_expression(
                    &table->spatial, kind, table->spatial_format,
                    field->name, field->name_length, &expression) !=
                    VPS_SPATIAL_OK ||
                !vps_append_bytes(&projected, expression.sql,
                                  expression.length) ||
                !vps_append_bytes(&projected, " AS ", 4U) ||
                !vps_append_pg_identifier(&projected, field->name,
                                          field->name_length))
                goto fail;
            field->type_oid = expression.binary_result
                                  ? UINT32_C(17) : UINT32_C(25);
            field->type_modifier = -1;
        } else if (!vps_append_pg_identifier(&projected, field->name,
                                             field->name_length))
            goto fail;
    }
    if (!vps_append_bytes(&projected, " FROM (", 7U) ||
        !vps_append_bytes(&projected, (const char *)table->scan_query.data,
                          table->scan_query.size) ||
        !vps_append_bytes(&projected, ") AS vps_spatial_source", 23U) ||
        !vps_append_bytes(&projected, "\0", 1U))
        goto fail;
    projected.size -= 1U;
    vps_buffer_reset(&table->scan_query);
    table->scan_query = projected;
    return 1;
fail:
    vps_buffer_reset(&projected);
    return 0;
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
    if (table->initialized_table_metadata) {
        size_t key_index;
        for (key_index = 0U;
             key_index < table->table_metadata.key.column_count; ++key_index) {
            size_t field;
            int32_t attribute_number =
                table->table_metadata.key.attribute_numbers[key_index];
            for (field = 0U; field < table->described.field_count; ++field) {
                if (table->described.fields[field].origin_attribute_number ==
                    attribute_number)
                    break;
            }
            if (field == table->described.field_count)
                return 0;
            table->key_columns[table->key_column_count++] = (uint16_t)field;
        }
        if (table->key_column_count == 0U) return table->mode_rw ? 0 : 1;
        if (table->key_column_count == 1U &&
            table->optimistic_mode == VPS_DML_OPTIMISTIC_OFF &&
            vps_type_codec_for_oid(
                table->described.fields[table->key_columns[0]].type_oid) ==
                VPS_CODEC_INTEGER) {
            table->identity_mode = VPS_ROW_IDENTITY_STABLE_INTEGER;
            table->has_hidden_identity = 0;
        } else {
            table->identity_mode = VPS_ROW_IDENTITY_HIDDEN_TOKEN;
            table->has_hidden_identity = 1;
        }
        return 1;
    }
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
    table->has_hidden_identity =
        table->identity_mode == VPS_ROW_IDENTITY_HIDDEN_TOKEN;
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
    for (index = 0U; index < table->visible_field_count; ++index) {
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
    if (table->has_hidden_identity) {
        const char *identity_declaration =
            table->identity_mode == VPS_ROW_IDENTITY_HIDDEN_TOKEN
                ? ",\"__vps_identity\" BLOB HIDDEN PRIMARY KEY NOT NULL"
                : ",\"__vps_identity\" BLOB HIDDEN";
        if (!vps_append_bytes(&declaration, identity_declaration,
                              strlen(identity_declaration))) {
            vps_buffer_reset(&declaration);
            return SQLITE_NOMEM;
        }
    }
    if (table->mode_rw &&
        !vps_append_bytes(&declaration,
                          ",\"__vps_omit\" TEXT HIDDEN", 25U)) {
        vps_buffer_reset(&declaration);
        return SQLITE_NOMEM;
    }
    if (!vps_append_bytes(
            &declaration,
            table->identity_mode == VPS_ROW_IDENTITY_HIDDEN_TOKEN
                ? ") WITHOUT ROWID\0" : ")\0",
            table->identity_mode == VPS_ROW_IDENTITY_HIDDEN_TOKEN ? 16U : 2U)) {
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
        hash ^= table->spatial_kind[index];
        hash *= UINT64_C(1099511628211);
        hash ^= (uint32_t)table->spatial_type[index];
        hash *= UINT64_C(1099511628211);
        hash ^= table->spatial_dimensions[index];
        hash *= UINT64_C(1099511628211);
        hash ^= table->spatial_srid[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= table->spatial.namespace_oid;
    hash *= UINT64_C(1099511628211);
    hash ^= table->spatial.geometry_oid;
    hash *= UINT64_C(1099511628211);
    hash ^= table->spatial.geography_oid;
    hash *= UINT64_C(1099511628211);
    hash ^= table->spatial.flags;
    hash *= UINT64_C(1099511628211);
    hash ^= (uint32_t)table->spatial_format;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static void vps_table_description_reset(VpsTable *table)
{
    if (table->initialized_described) {
        vps_query_describe_result_cleanup(&table->described);
        table->initialized_described = 0;
    }
    if (table->initialized_table_metadata) {
        vps_table_metadata_reset(&table->table_metadata);
        table->initialized_table_metadata = 0;
    }
    if (table->initialized_spatial) {
        vps_spatial_capabilities_reset(&table->spatial);
        table->initialized_spatial = 0;
    }
    vps_memory_release(&table->allocator, (void **)&table->expected_fields,
                       table->expected_fields_size);
    table->expected_fields_size = 0U;
    table->visible_field_count = 0U;
    table->key_column_count = 0U;
    (void)memset(table->spatial_kind, 0, sizeof(table->spatial_kind));
    (void)memset(table->spatial_type, 0, sizeof(table->spatial_type));
    (void)memset(table->spatial_dimensions, 0,
                 sizeof(table->spatial_dimensions));
    (void)memset(table->spatial_srid, 0, sizeof(table->spatial_srid));
}

static void vps_snapshot_hash_bytes(uint64_t *hash,
                                    const void *bytes,
                                    size_t size)
{
    const unsigned char *value = (const unsigned char *)bytes;
    size_t index;
    for (index = 0U; index < size; ++index) {
        *hash ^= value[index];
        *hash *= UINT64_C(1099511628211);
    }
}

static void vps_snapshot_hash_u64(uint64_t *hash, uint64_t value)
{
    unsigned char bytes[8];
    size_t index;
    for (index = 0U; index < sizeof(bytes); ++index)
        bytes[index] = (unsigned char)(value >> (index * 8U));
    vps_snapshot_hash_bytes(hash, bytes, sizeof(bytes));
}

static int vps_table_snapshot_build(const VpsTable *table,
                                    VpsMetadataSnapshot *snapshot)
{
    size_t index;
    const char *identity_fingerprint;
    uint64_t identity_hash = UINT64_C(1469598103934665603);
    if (vps_metadata_snapshot_init(snapshot, &table->allocator) !=
            VPS_METADATA_CACHE_OK)
        return SQLITE_NOMEM;
    snapshot->visible_count = table->visible_field_count;
    snapshot->source_fingerprint = table->source_fingerprint;
    snapshot->layout_fingerprint = vps_vtab_source_fingerprint(table);
    snapshot->configuration_generation =
        table->identity.configuration_generation;
    if (vps_platform_monotonic_now_ms(vps_platform_current_operations(),
                                      &snapshot->captured_at_ms) ==
        VPS_PLATFORM_OK)
        snapshot->validated_at_ms = snapshot->captured_at_ms;
    identity_fingerprint = vps_identity_fingerprint(&table->identity);
    if (identity_fingerprint != NULL) {
        for (index = 0U; identity_fingerprint[index] != '\0'; ++index) {
            identity_hash ^= (unsigned char)identity_fingerprint[index];
            identity_hash *= UINT64_C(1099511628211);
        }
    }
    snapshot->connection_identity_hash = identity_hash;
    snapshot->relation_oid = table->initialized_table_metadata
        ? table->table_metadata.relation.relation_oid
        : (table->described.field_count != 0U
               ? table->described.fields[0].origin_relation_oid : 0U);
    snapshot->relation_policy_fingerprint =
        UINT64_C(1469598103934665603);
    if (table->initialized_table_metadata) {
        const VpsRelationMetadata *relation = &table->table_metadata.relation;
        const VpsRelationPolicyMetadata *policy = &table->table_metadata.policy;
        vps_snapshot_hash_u64(&snapshot->relation_policy_fingerprint,
                              (uint64_t)relation->kind);
        vps_snapshot_hash_u64(&snapshot->relation_policy_fingerprint,
                              (unsigned char)relation->persistence);
        vps_snapshot_hash_u64(&snapshot->relation_policy_fingerprint,
                              (uint64_t)relation->is_partition);
        vps_snapshot_hash_u64(&snapshot->relation_policy_fingerprint,
                              (uint64_t)relation->row_security);
        vps_snapshot_hash_u64(&snapshot->relation_policy_fingerprint,
                              (uint64_t)relation->force_row_security);
        vps_snapshot_hash_u64(&snapshot->relation_policy_fingerprint,
                              (uint64_t)policy->write_policy);
        for (index = 0U; index < policy->parent_count; ++index)
            vps_snapshot_hash_u64(&snapshot->relation_policy_fingerprint,
                                  policy->parent_oids[index]);
        for (index = 0U; index < policy->partition_attribute_count; ++index)
            vps_snapshot_hash_u64(
                &snapshot->relation_policy_fingerprint,
                (uint32_t)policy->partition_attribute_numbers[index]);
    }
    snapshot->spatial_namespace_oid = table->spatial.namespace_oid;
    snapshot->spatial_geometry_oid = table->spatial.geometry_oid;
    snapshot->spatial_geography_oid = table->spatial.geography_oid;
    snapshot->spatial_flags = table->spatial.flags;
    snapshot->spatial_format = (uint32_t)table->spatial_format;
    snapshot->key_count = table->key_column_count;
    for (index = 0U; index < table->key_column_count; ++index)
        snapshot->key_columns[index] = table->key_columns[index];
    for (index = 0U; index < table->described.field_count; ++index) {
        const VpsQueryDescribeField *source = &table->described.fields[index];
        VpsMetadataCacheField field;
        (void)memset(&field, 0, sizeof(field));
        field.type_oid = source->type_oid;
        field.type_modifier = source->type_modifier;
        field.origin_relation_oid = source->origin_relation_oid;
        field.origin_attribute_number = source->origin_attribute_number;
        field.spatial_kind = table->spatial_kind[index];
        field.spatial_type = (uint8_t)table->spatial_type[index];
        field.spatial_dimensions = (uint8_t)table->spatial_dimensions[index];
        field.spatial_srid = table->spatial_srid[index];
        field.policy_fingerprint = UINT64_C(1469598103934665603);
        if (table->initialized_table_metadata &&
            source->origin_relation_oid ==
                table->table_metadata.relation.relation_oid) {
            size_t metadata_index;
            for (metadata_index = 0U;
                 metadata_index < table->table_metadata.columns.column_count;
                 ++metadata_index) {
                const VpsColumnMetadata *column =
                    &table->table_metadata.columns.columns[metadata_index];
                const VpsMetadataString *strings[4];
                size_t string_index;
                if (column->attribute_number !=
                    source->origin_attribute_number) continue;
                field.collation_oid = column->collation_oid;
                vps_snapshot_hash_u64(&field.policy_fingerprint,
                                      (uint64_t)column->not_null);
                vps_snapshot_hash_u64(&field.policy_fingerprint,
                                      (uint64_t)column->has_default);
                vps_snapshot_hash_u64(&field.policy_fingerprint,
                                      (unsigned char)column->generated_kind);
                vps_snapshot_hash_u64(&field.policy_fingerprint,
                                      (unsigned char)column->identity_kind);
                vps_snapshot_hash_u64(&field.policy_fingerprint,
                                      (unsigned char)column->storage_kind);
                vps_snapshot_hash_u64(&field.policy_fingerprint,
                                      (unsigned char)column->compression_kind);
                vps_snapshot_hash_u64(&field.policy_fingerprint,
                                      (uint64_t)column->domain_not_null);
                strings[0] = &column->default_expression_hash;
                strings[1] = &column->domain_default_hash;
                strings[2] = &column->domain_constraint_hash;
                strings[3] = &column->formatted_type;
                for (string_index = 0U; string_index < 4U; ++string_index) {
                    const unsigned char *value = NULL;
                    size_t length = 0U;
                    if (vps_column_set_string(
                            &table->table_metadata.columns,
                            strings[string_index], &value, &length) ==
                            VPS_METADATA_OK && value != NULL)
                        vps_snapshot_hash_bytes(&field.policy_fingerprint,
                                                value, length);
                }
                break;
            }
        }
        if (vps_metadata_snapshot_add_field(
                snapshot, source->name, source->name_length, &field) !=
                VPS_METADATA_CACHE_OK) {
            vps_metadata_snapshot_reset(snapshot);
            return SQLITE_NOMEM;
        }
    }
    return vps_metadata_snapshot_validate(snapshot) == VPS_METADATA_CACHE_OK
               ? SQLITE_OK : SQLITE_CORRUPT;
}

static char *vps_shadow_qualified(const VpsTable *table, const char *suffix)
{
    if (table == NULL || table->shadow_schema == NULL ||
        table->shadow_name == NULL || suffix == NULL) return NULL;
    return sqlite3_mprintf("\"%w\".\"%w%s\"", table->shadow_schema,
                           table->shadow_name, suffix);
}

static int vps_shadow_store(VpsTable *table,
                            const VpsMetadataSnapshot *snapshot)
{
    VpsBuffer encoded = {0};
    sqlite3_stmt *statement = NULL;
    char *schema_table = NULL;
    char *metadata_table = NULL;
    char *sql = NULL;
    char source[17];
    char layout[17];
    int result = SQLITE_ERROR;
    if (vps_metadata_snapshot_encode(snapshot, &encoded) !=
        VPS_METADATA_CACHE_OK) return SQLITE_NOMEM;
    schema_table = vps_shadow_qualified(table, "_vps_schema");
    metadata_table = vps_shadow_qualified(table, "_vps_metadata");
    if (schema_table == NULL || metadata_table == NULL) goto cleanup;
    sql = sqlite3_mprintf(
        "CREATE TABLE IF NOT EXISTS %s(format_version INTEGER NOT NULL,source_fingerprint TEXT NOT NULL,layout_fingerprint TEXT NOT NULL,relation_oid INTEGER NOT NULL,configuration_generation INTEGER NOT NULL,captured_at INTEGER NOT NULL,codec_version INTEGER NOT NULL,extension_version TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS %s(snapshot BLOB NOT NULL,last_validation INTEGER NOT NULL);"
        "DELETE FROM %s;DELETE FROM %s;",
        schema_table, metadata_table, schema_table, metadata_table);
    if (sql == NULL) { result = SQLITE_NOMEM; goto cleanup; }
    result = sqlite3_exec(table->database, sql, NULL, NULL, NULL);
    sqlite3_free(sql); sql = NULL;
    if (result != SQLITE_OK) goto cleanup;
    (void)snprintf(source, sizeof(source), "%016llx",
                   (unsigned long long)snapshot->source_fingerprint);
    (void)snprintf(layout, sizeof(layout), "%016llx",
                   (unsigned long long)snapshot->layout_fingerprint);
    sql = sqlite3_mprintf("INSERT INTO %s VALUES(?,?,?,?,?,?,?,?)",
                          schema_table);
    if (sql == NULL) { result = SQLITE_NOMEM; goto cleanup; }
    result = sqlite3_prepare_v2(table->database, sql, -1, &statement, NULL);
    if (result == SQLITE_OK) {
        sqlite3_bind_int(statement, 1, (int)VPS_METADATA_CACHE_VERSION);
        sqlite3_bind_text(statement, 2, source, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, layout, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 4, snapshot->relation_oid);
        sqlite3_bind_int64(statement, 5,
                           (sqlite3_int64)snapshot->configuration_generation);
        sqlite3_bind_int64(statement, 6,
                           (sqlite3_int64)snapshot->captured_at_ms);
        sqlite3_bind_int(statement, 7, (int)VPS_METADATA_CACHE_VERSION);
        sqlite3_bind_text(statement, 8, "0.9.0", -1, SQLITE_STATIC);
        result = sqlite3_step(statement) == SQLITE_DONE ? SQLITE_OK
                                                        : SQLITE_ERROR;
    }
    sqlite3_finalize(statement); statement = NULL;
    sqlite3_free(sql); sql = NULL;
    if (result != SQLITE_OK) goto cleanup;
    sql = sqlite3_mprintf("INSERT INTO %s VALUES(?,?)", metadata_table);
    if (sql == NULL) { result = SQLITE_NOMEM; goto cleanup; }
    result = sqlite3_prepare_v2(table->database, sql, -1, &statement, NULL);
    if (result == SQLITE_OK && encoded.size <= INT_MAX) {
        sqlite3_bind_blob(statement, 1, encoded.data, (int)encoded.size,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 2,
                           (sqlite3_int64)snapshot->validated_at_ms);
        result = sqlite3_step(statement) == SQLITE_DONE ? SQLITE_OK
                                                        : SQLITE_ERROR;
    }
cleanup:
    sqlite3_finalize(statement);
    sqlite3_free(sql); sqlite3_free(schema_table); sqlite3_free(metadata_table);
    vps_buffer_reset(&encoded);
    return result;
}

static int vps_shadow_load(VpsTable *table, VpsMetadataSnapshot *snapshot)
{
    sqlite3_stmt *statement = NULL;
    char *schema_table = vps_shadow_qualified(table, "_vps_schema");
    char *metadata_table = vps_shadow_qualified(table, "_vps_metadata");
    char *sql = NULL;
    VpsBuffer bytes = {0};
    char schema_source[17] = {0};
    char schema_layout[17] = {0};
    sqlite3_int64 schema_relation = 0;
    sqlite3_int64 schema_generation = 0;
    sqlite3_int64 schema_captured = 0;
    sqlite3_int64 metadata_validated = 0;
    int result = SQLITE_CORRUPT;
    int step;
    if (schema_table == NULL || metadata_table == NULL) {
        result = SQLITE_NOMEM; goto cleanup;
    }
    sql = sqlite3_mprintf(
        "SELECT format_version,source_fingerprint,layout_fingerprint,"
        "relation_oid,configuration_generation,captured_at,codec_version,"
        "extension_version FROM %s", schema_table);
    if (sql == NULL) { result = SQLITE_NOMEM; goto cleanup; }
    if (sqlite3_prepare_v2(table->database, sql, -1, &statement, NULL) !=
        SQLITE_OK) { result = SQLITE_NOTFOUND; goto cleanup; }
    step = sqlite3_step(statement);
    if (step != SQLITE_ROW || sqlite3_column_count(statement) != 8 ||
        sqlite3_column_int(statement, 0) != (int)VPS_METADATA_CACHE_VERSION ||
        sqlite3_column_type(statement, 1) != SQLITE_TEXT ||
        sqlite3_column_bytes(statement, 1) != 16 ||
        sqlite3_column_type(statement, 2) != SQLITE_TEXT ||
        sqlite3_column_bytes(statement, 2) != 16 ||
        sqlite3_column_int(statement, 6) != (int)VPS_METADATA_CACHE_VERSION ||
        sqlite3_column_type(statement, 7) != SQLITE_TEXT ||
        sqlite3_column_bytes(statement, 7) != 5 ||
        memcmp(sqlite3_column_text(statement, 7), "0.9.0", 5U) != 0)
        goto cleanup;
    (void)memcpy(schema_source, sqlite3_column_text(statement, 1), 16U);
    (void)memcpy(schema_layout, sqlite3_column_text(statement, 2), 16U);
    schema_relation = sqlite3_column_int64(statement, 3);
    schema_generation = sqlite3_column_int64(statement, 4);
    schema_captured = sqlite3_column_int64(statement, 5);
    if (sqlite3_step(statement) != SQLITE_DONE) goto cleanup;
    sqlite3_finalize(statement); statement = NULL;
    sqlite3_free(sql); sql = NULL;
    sql = sqlite3_mprintf("SELECT snapshot,last_validation FROM %s",
                          metadata_table);
    if (sql == NULL) { result = SQLITE_NOMEM; goto cleanup; }
    if (sqlite3_prepare_v2(table->database, sql, -1, &statement, NULL) !=
        SQLITE_OK) { result = SQLITE_NOTFOUND; goto cleanup; }
    step = sqlite3_step(statement);
    if (step != SQLITE_ROW || sqlite3_column_count(statement) != 2 ||
        sqlite3_column_type(statement, 0) != SQLITE_BLOB ||
        sqlite3_column_bytes(statement, 0) <= 0 ||
        sqlite3_column_bytes(statement, 0) >
            (int)VPS_METADATA_CACHE_MAX_BYTES ||
        vps_buffer_init(&bytes, &table->allocator,
                        VPS_METADATA_CACHE_MAX_BYTES) != VPS_MEMORY_OK ||
        vps_buffer_append(&bytes, sqlite3_column_blob(statement, 0),
                          (size_t)sqlite3_column_bytes(statement, 0)) !=
            VPS_MEMORY_OK) {
        result = SQLITE_CORRUPT; goto cleanup;
    }
    metadata_validated = sqlite3_column_int64(statement, 1);
    if (sqlite3_step(statement) != SQLITE_DONE) goto cleanup;
    result = vps_metadata_snapshot_decode(snapshot, &table->allocator,
                                          bytes.data, bytes.size) ==
                 VPS_METADATA_CACHE_OK ? SQLITE_OK : SQLITE_CORRUPT;
    if (result == SQLITE_OK) {
        char expected_source[17];
        char expected_layout[17];
        (void)snprintf(expected_source, sizeof(expected_source), "%016llx",
                       (unsigned long long)snapshot->source_fingerprint);
        (void)snprintf(expected_layout, sizeof(expected_layout), "%016llx",
                       (unsigned long long)snapshot->layout_fingerprint);
        if (memcmp(schema_source, expected_source, 16U) != 0 ||
            memcmp(schema_layout, expected_layout, 16U) != 0 ||
            schema_relation != (sqlite3_int64)snapshot->relation_oid ||
            schema_generation !=
                (sqlite3_int64)snapshot->configuration_generation ||
            schema_captured != (sqlite3_int64)snapshot->captured_at_ms ||
            metadata_validated != (sqlite3_int64)snapshot->validated_at_ms)
            result = SQLITE_CORRUPT;
    }
cleanup:
    sqlite3_finalize(statement); sqlite3_free(sql);
    sqlite3_free(schema_table); sqlite3_free(metadata_table);
    vps_buffer_reset(&bytes);
    return result;
}

static int vps_table_snapshot_hydrate(VpsTable *table,
                                      const VpsMetadataSnapshot *snapshot)
{
    size_t allocation_size;
    size_t index;
    if (vps_size_multiply(snapshot->field_count,
                          sizeof(*table->described.fields),
                          &allocation_size) != VPS_MEMORY_OK ||
        vps_query_describe_result_init(&table->described, &table->allocator) !=
            VPS_QUERY_VALIDATION_OK ||
        vps_memory_allocate(&table->allocator, allocation_size,
                            (void **)&table->described.fields) != VPS_MEMORY_OK)
        return SQLITE_NOMEM;
    table->initialized_described = 1;
    table->described.allocation_size = allocation_size;
    table->described.field_count = snapshot->field_count;
    (void)memset(table->described.fields, 0, allocation_size);
    for (index = 0U; index < snapshot->field_count; ++index) {
        const VpsMetadataCacheField *field = &snapshot->fields[index];
        VpsQueryDescribeField *destination = &table->described.fields[index];
        size_t name_length;
        const char *name = vps_metadata_snapshot_field_name(
            snapshot, index, &name_length);
        if (name == NULL || name_length > VPS_CLIENT_MAX_FIELD_NAME_BYTES)
            return SQLITE_CORRUPT;
        (void)memcpy(destination->name, name, name_length);
        destination->name[name_length] = '\0';
        destination->name_length = name_length;
        destination->type_oid = field->type_oid;
        destination->type_modifier = field->type_modifier;
        destination->origin_relation_oid = field->origin_relation_oid;
        destination->origin_attribute_number = field->origin_attribute_number;
        destination->format = VPS_CLIENT_VALUE_TEXT;
        table->spatial_kind[index] = field->spatial_kind;
        table->spatial_type[index] =
            (VpsSpatialGeometryType)field->spatial_type;
        table->spatial_dimensions[index] = field->spatial_dimensions;
        table->spatial_srid[index] = field->spatial_srid;
    }
    table->visible_field_count = snapshot->visible_count;
    table->key_column_count = snapshot->key_count;
    for (index = 0U; index < snapshot->key_count; ++index)
        table->key_columns[index] = snapshot->key_columns[index];
    table->source_fingerprint = snapshot->source_fingerprint;
    table->spatial.namespace_oid = snapshot->spatial_namespace_oid;
    table->spatial.geometry_oid = snapshot->spatial_geometry_oid;
    table->spatial.geography_oid = snapshot->spatial_geography_oid;
    table->spatial.flags = snapshot->spatial_flags;
    table->spatial.present = snapshot->spatial_geometry_oid != 0U ||
                             snapshot->spatial_geography_oid != 0U;
    table->spatial_format = (VpsSpatialFormat)snapshot->spatial_format;
    return SQLITE_OK;
}

static int vps_module_connect_common(sqlite3 *database,
                                     void *auxiliary,
                                     int argument_count,
                                     const char *const *arguments,
                                     sqlite3_vtab **table_out,
                                     char **error_out,
                                     int creating)
{
    VpsTable *table;
    VpsMetadataSnapshot stored_snapshot = {0};
    VpsMetadataSnapshot live_snapshot = {0};
    VpsError error;
    int error_initialized = 0;
    int result;
    const char *failure;
    if (database == NULL || table_out == NULL) return SQLITE_MISUSE;
    *table_out = NULL;
    table = (VpsTable *)sqlite3_malloc64(sizeof(*table));
    if (table == NULL) return SQLITE_NOMEM;
    (void)memset(table, 0, sizeof(*table));
    table->database = database;
    table->shadow_schema = argument_count >= 3
        ? sqlite3_mprintf("%s", arguments[1]) : NULL;
    table->shadow_name = argument_count >= 3
        ? sqlite3_mprintf("%s", arguments[2]) : NULL;
    if (table->shadow_schema == NULL || table->shadow_name == NULL) {
        sqlite3_free(table->shadow_schema);
        sqlite3_free(table->shadow_name);
        sqlite3_free(table);
        return SQLITE_NOMEM;
    }
    table->module_context = (VpsModuleContext *)auxiliary;
    if (table->module_context == NULL || table->module_context->closing) {
        sqlite3_free(table->shadow_schema);
        sqlite3_free(table->shadow_name);
        sqlite3_free(table);
        return SQLITE_MISUSE;
    }
    table->table_id = ++table->module_context->next_table_id;
    table->module_context->table_references += 1U;
    result = vps_table_initialize_runtime(table, argument_count, arguments,
                                          &error);
    if (vps_error_init(&error, &table->allocator) == VPS_MEMORY_OK)
        error_initialized = 1;
    failure = "runtime initialization";
    if (result == SQLITE_OK) {
        failure = "source describe";
        result = vps_table_describe(table, &error);
    }
    if (result != SQLITE_OK && !creating && table->metadata_cached &&
        error_initialized &&
        vps_metadata_cache_fallback_allowed(error.error_class)) {
        vps_table_description_reset(table);
        result = vps_shadow_load(table, &stored_snapshot);
        if (result == SQLITE_OK) {
            const char *fingerprint = vps_identity_fingerprint(&table->identity);
            uint64_t hash = UINT64_C(1469598103934665603);
            size_t index;
            if (fingerprint != NULL)
                for (index = 0U; fingerprint[index] != '\0'; ++index) {
                    hash ^= (unsigned char)fingerprint[index];
                    hash *= UINT64_C(1099511628211);
                }
            if (hash != stored_snapshot.connection_identity_hash)
                result = SQLITE_AUTH;
        }
        if (result == SQLITE_OK)
            result = vps_table_snapshot_hydrate(table, &stored_snapshot);
        if (result == SQLITE_OK) {
            table->cache_fallback = 1;
            failure = "cached metadata declaration";
        }
    }
    if (result == SQLITE_OK) {
        table->visible_field_count = table->described.field_count;
        if (table->optimistic_mode == VPS_DML_OPTIMISTIC_XMIN) {
            if (table->visible_field_count == 0U)
                result = SQLITE_ERROR;
            else
                table->visible_field_count -= 1U;
        }
    }
    if (result == SQLITE_OK && !table->cache_fallback) {
        failure = "spatial integration";
        if (!vps_table_integrate_spatial(table)) result = SQLITE_ERROR;
    }
    if (result == SQLITE_OK) {
        failure = "DML policy";
        if (!vps_table_build_dml_policy(table)) result = SQLITE_ERROR;
    }
    if (result == SQLITE_OK) {
        failure = "expected fields";
        if (!vps_build_expected_fields(table)) result = SQLITE_NOMEM;
    }
    if (result == SQLITE_OK) {
        failure = "row identity";
        if (!vps_resolve_row_identity(table)) result = SQLITE_ERROR;
    }
    if (result == SQLITE_OK &&
        table->visible_field_count > VPS_PLAN_MAX_COLUMNS)
        result = SQLITE_TOOBIG;
    if (result == SQLITE_OK && !table->cache_fallback)
        table->source_fingerprint = vps_vtab_source_fingerprint(table);
    if (result == SQLITE_OK && !table->cache_fallback) {
        failure = "metadata snapshot";
        result = vps_table_snapshot_build(table, &live_snapshot);
    }
    if (result == SQLITE_OK && !creating && !table->cache_fallback) {
        VpsMetadataDrift drift;
        failure = "metadata drift validation";
        result = vps_shadow_load(table, &stored_snapshot);
        if (result == SQLITE_OK) {
            drift = vps_metadata_snapshot_compare(&stored_snapshot,
                                                  &live_snapshot);
            if (drift == VPS_METADATA_DRIFT_INCOMPATIBLE ||
                (drift == VPS_METADATA_DRIFT_REFRESHABLE &&
                 !table->schema_refresh))
                result = SQLITE_SCHEMA;
            else if (drift == VPS_METADATA_DRIFT_REFRESHABLE)
                result = vps_shadow_store(table, &live_snapshot);
        }
    }
#if defined(VPS_ENABLE_QUERY_MATERIALIZATION)
    if (result == SQLITE_OK && !vps_table_materialization_init(table))
        result = SQLITE_ERROR;
#endif
    if (result == SQLITE_OK) {
        failure = "SQLite schema declaration";
        result = vps_declare_schema(database, table);
    }
    if (result == SQLITE_OK && creating) {
        failure = "shadow metadata write";
        result = vps_shadow_store(table, &live_snapshot);
    }
    if (result != SQLITE_OK) {
        if (error_out != NULL) {
            const char *message = error_initialized
                                      ? vps_error_message(&error) : NULL;
            *error_out = message != NULL
                ? sqlite3_mprintf("%s", message)
                : sqlite3_mprintf("VirtualPostgreSQL %s failed", failure);
        }
        if (error_initialized) vps_error_reset(&error);
        vps_metadata_snapshot_reset(&stored_snapshot);
        vps_metadata_snapshot_reset(&live_snapshot);
        (void)vps_table_cleanup(table);
        return result;
    }
    if (error_initialized) vps_error_reset(&error);
    vps_metadata_snapshot_reset(&stored_snapshot);
    vps_metadata_snapshot_reset(&live_snapshot);
    *table_out = &table->base;
    return SQLITE_OK;
}

static int vps_module_create(sqlite3 *database, void *auxiliary,
                             int argument_count,
                             const char *const *arguments,
                             sqlite3_vtab **table_out, char **error_out)
{
    return vps_module_connect_common(database, auxiliary, argument_count,
                                     arguments, table_out, error_out, 1);
}

static int vps_module_connect(sqlite3 *database, void *auxiliary,
                              int argument_count,
                              const char *const *arguments,
                              sqlite3_vtab **table_out, char **error_out)
{
    return vps_module_connect_common(database, auxiliary, argument_count,
                                     arguments, table_out, error_out, 0);
}

static int vps_module_disconnect(sqlite3_vtab *base)
{
    return vps_table_cleanup((VpsTable *)base);
}

static int vps_module_destroy(sqlite3_vtab *base)
{
    VpsTable *table = (VpsTable *)base;
    char *schema_table;
    char *metadata_table;
    char *sql;
    int result;
    if (table == NULL) return SQLITE_OK;
    schema_table = vps_shadow_qualified(table, "_vps_schema");
    metadata_table = vps_shadow_qualified(table, "_vps_metadata");
    if (schema_table == NULL || metadata_table == NULL) {
        sqlite3_free(schema_table); sqlite3_free(metadata_table);
        (void)vps_table_cleanup(table);
        return SQLITE_NOMEM;
    }
    sql = sqlite3_mprintf("DROP TABLE IF EXISTS %s;DROP TABLE IF EXISTS %s",
                          metadata_table, schema_table);
    sqlite3_free(schema_table); sqlite3_free(metadata_table);
    if (sql == NULL) {
        (void)vps_table_cleanup(table);
        return SQLITE_NOMEM;
    }
    result = sqlite3_exec(table->database, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    if (vps_table_cleanup(table) != SQLITE_OK && result == SQLITE_OK)
        result = SQLITE_ERROR;
    return result;
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
        vtab->visible_field_count > VPS_PLAN_MAX_COLUMNS ||
        index_info->nConstraint < 0 ||
        (size_t)index_info->nConstraint > VPS_PLAN_MAX_CONSTRAINTS ||
        index_info->nOrderBy < 0 ||
        (size_t)index_info->nOrderBy > VPS_PLAN_MAX_ORDER_TERMS)
        return SQLITE_CONSTRAINT;
    (void)memset(columns, 0, sizeof(columns));
    (void)memset(constraints, 0, sizeof(constraints));
    (void)memset(order_terms, 0, sizeof(order_terms));
    for (index = 0U; index < vtab->visible_field_count; ++index) {
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
                vtab->visible_field_count)
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
    request.column_count = vtab->visible_field_count;
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
    if (vps_planner_compile(&request, &plan) != VPS_PLANNER_OK)
        return SQLITE_ERROR;
    if (vtab->optimistic_mode == VPS_DML_OPTIMISTIC_COLUMN) {
        size_t projection;
        int present = 0;
        for (projection = 0U; projection < plan.projection_count; ++projection)
            if (plan.projection[projection] ==
                (uint16_t)vtab->dml_policy.version_visible)
                present = 1;
        if (!present) {
            if (plan.projection_count >= VPS_PLAN_MAX_COLUMNS)
                return SQLITE_TOOBIG;
            plan.projection[plan.projection_count++] =
                (uint16_t)vtab->dml_policy.version_visible;
        }
    }
    if (vps_plan_encode(&plan, &vtab->allocator, &encoded) != VPS_PLANNER_OK)
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
    if (cursor->transaction_stream) {
        if (vps_transaction_stream_end(
                &cursor->table->module_context->transaction) !=
            VPS_TRANSACTION_OK)
            clean = 0;
        cursor->transaction_stream = 0;
    }
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

static int vps_cursor_build_identity(VpsCursor *cursor,
                                     const VpsClientRowView *row,
                                     VpsError *error)
{
    VpsRowIdentityField keys[VPS_QUERY_METADATA_MAX_KEY_COLUMNS];
    VpsRowIdentityField optimistic;
    VpsRowIdentitySpec spec;
    size_t key_index;
    (void)memset(keys, 0, sizeof(keys));
    (void)memset(&optimistic, 0, sizeof(optimistic));
    (void)memset(&spec, 0, sizeof(spec));
    for (key_index = 0U;
         key_index < cursor->table->key_column_count; ++key_index) {
        size_t logical = cursor->table->key_columns[key_index];
        VpsClientColumnView column;
        if (logical >= VPS_PLAN_MAX_COLUMNS ||
            cursor->logical_to_remote[logical] == UINT16_MAX ||
            vps_client_row_column(row, cursor->logical_to_remote[logical],
                                  &column, error) != VPS_CLIENT_OK)
            return 0;
        keys[key_index].kind = column.is_null
            ? VPS_ROW_IDENTITY_FIELD_NULL
            : (vps_type_codec_for_oid(column.type_oid) == VPS_CODEC_BYTEA
                   ? VPS_ROW_IDENTITY_FIELD_BLOB
                   : VPS_ROW_IDENTITY_FIELD_TEXT);
        keys[key_index].type_oid =
            cursor->table->dml_policy.selections[logical].declared_type_oid;
        keys[key_index].bytes = column.data;
        keys[key_index].length = column.length;
    }
    spec.relation_oid =
        cursor->table->table_metadata.relation.relation_oid;
    spec.key_fields = keys;
    spec.key_field_count = cursor->table->key_column_count;
    if (cursor->table->optimistic_mode != VPS_DML_OPTIMISTIC_OFF) {
        VpsClientColumnView column;
        if (cursor->optimistic_remote_column == SIZE_MAX ||
            vps_client_row_column(row, cursor->optimistic_remote_column,
                                  &column, error) != VPS_CLIENT_OK ||
            column.is_null)
            return 0;
        optimistic.kind = VPS_ROW_IDENTITY_FIELD_TEXT;
        optimistic.type_oid =
            cursor->table->optimistic_mode == VPS_DML_OPTIMISTIC_XMIN
                ? UINT32_C(28)
                : cursor->table->dml_policy.selections[
                      cursor->table->dml_policy.version_visible]
                      .declared_type_oid;
        optimistic.bytes = column.data;
        optimistic.length = column.length;
        spec.optimistic_field = &optimistic;
    }
    return vps_row_identity_encode(&cursor->table->allocator, &spec,
                                   &cursor->identity_token) ==
           VPS_ROW_IDENTITY_OK;
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
        if (vps_client_statement_close(&cursor->statement) != VPS_CLIENT_OK)
            goto failed;
        if (cursor->transaction_stream) {
            (void)vps_transaction_stream_end(
                &cursor->table->module_context->transaction);
            cursor->transaction_stream = 0;
        }
        return SQLITE_OK;
    }
    status = vps_client_statement_current_row(cursor->statement, &row, error);
    if (status != VPS_CLIENT_OK ||
        vps_client_row_column_count(row) != cursor->remote_projection_count)
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
        if (index >= cursor->plan.projection_count) continue;
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
        if (!vps_cursor_build_identity(cursor, row, error))
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
    if (cursor->transaction_stream) {
        (void)vps_transaction_mark_failed(
            &cursor->table->module_context->transaction,
            error != NULL && error->sqlstate[0] != '\0' ? error->sqlstate
                                                        : NULL);
        (void)vps_transaction_stream_end(
            &cursor->table->module_context->transaction);
        cursor->transaction_stream = 0;
    }
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
    cursor->remote_projection_count = cursor->plan.projection_count +
        (table->optimistic_mode == VPS_DML_OPTIMISTIC_XMIN ? 1U : 0U);
    cursor->optimistic_remote_column = SIZE_MAX;
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
        vps_size_multiply(cursor->remote_projection_count,
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
    if (table->optimistic_mode == VPS_DML_OPTIMISTIC_XMIN) {
        size_t internal = table->visible_field_count;
        if (!vps_append_bytes(&cursor->planned_query, ",", 1U) ||
            !vps_vtab_append_column_name(&cursor->planned_query, table,
                                         internal))
            goto cleanup;
        cursor->optimistic_remote_column = cursor->plan.projection_count;
        cursor->projected_fields[cursor->optimistic_remote_column] =
            table->expected_fields[internal];
    } else if (table->optimistic_mode == VPS_DML_OPTIMISTIC_COLUMN) {
        size_t version = table->dml_policy.version_visible;
        cursor->optimistic_remote_column = cursor->logical_to_remote[version];
        if (cursor->optimistic_remote_column == UINT16_MAX) goto cleanup;
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

static int vps_table_validate_cached_metadata(VpsTable *table)
{
    VpsMetadataSnapshot stored = {0};
    VpsMetadataSnapshot live = {0};
    VpsError error;
    VpsMetadataDrift drift;
    int result;
    if (table == NULL || !table->cache_fallback) return SQLITE_OK;
    result = vps_shadow_load(table, &stored);
    if (result != SQLITE_OK) goto cleanup;
    vps_table_description_reset(table);
    if (vps_error_init(&error, &table->allocator) != VPS_MEMORY_OK) {
        result = SQLITE_NOMEM; goto cleanup;
    }
    result = vps_table_describe(table, &error);
    if (result == SQLITE_OK) {
        table->visible_field_count = table->described.field_count;
        result = vps_table_integrate_spatial(table) ? SQLITE_OK : SQLITE_ERROR;
    }
    if (result == SQLITE_OK && !vps_table_build_dml_policy(table))
        result = SQLITE_ERROR;
    if (result == SQLITE_OK && !vps_build_expected_fields(table))
        result = SQLITE_NOMEM;
    if (result == SQLITE_OK && !vps_resolve_row_identity(table))
        result = SQLITE_ERROR;
    if (result == SQLITE_OK) {
        table->source_fingerprint = vps_vtab_source_fingerprint(table);
        result = vps_table_snapshot_build(table, &live);
    }
    if (result == SQLITE_OK) {
        drift = vps_metadata_snapshot_compare(&stored, &live);
        if (drift == VPS_METADATA_DRIFT_INCOMPATIBLE ||
            (drift == VPS_METADATA_DRIFT_REFRESHABLE &&
             !table->schema_refresh))
            result = SQLITE_SCHEMA;
        else if (drift == VPS_METADATA_DRIFT_REFRESHABLE) {
            result = vps_shadow_store(table, &live);
            if (result == SQLITE_OK) result = SQLITE_SCHEMA;
        } else table->cache_fallback = 0;
    }
    vps_error_reset(&error);
cleanup:
    vps_metadata_snapshot_reset(&stored);
    vps_metadata_snapshot_reset(&live);
    return result;
}

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
    if (table->cache_fallback) {
        result = vps_table_validate_cached_metadata(table);
        if (result != SQLITE_OK)
            return vps_vtab_set_error(
                &table->base, result, NULL,
                result == SQLITE_SCHEMA
                    ? "VirtualPostgreSQL cached schema changed"
                    : "VirtualPostgreSQL live metadata validation failed");
    }
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
    {
        VpsTransactionStatus transaction_status;
        VpsTransactionResult transaction_result = vps_transaction_status(
            &table->module_context->transaction, &transaction_status);
        if (transaction_result == VPS_TRANSACTION_OK &&
            transaction_status.state != VPS_TRANSACTION_IDLE) {
            transaction_result = vps_transaction_begin(
                &table->module_context->transaction, &table->identity,
                table->table_id);
            if (transaction_result == VPS_TRANSACTION_OK)
                transaction_result = vps_transaction_stream_begin(
                    &table->module_context->transaction);
            if (transaction_result == VPS_TRANSACTION_OK &&
                table->module_context->transaction_connection != NULL) {
                cursor->connection =
                    table->module_context->transaction_connection;
                cursor->transaction_stream = 1;
                status = VPS_CLIENT_OK;
            } else {
                return vps_vtab_set_error(
                    &table->base,
                    transaction_result == VPS_TRANSACTION_BUSY
                        ? SQLITE_BUSY : SQLITE_ABORT,
                    NULL, "VirtualPostgreSQL transaction stream unavailable");
            }
        } else {
            status = vps_connection_pool_acquire(
                         table->pool, &table->pool_key, 10000U, NULL, NULL,
                         &cursor->lease) == VPS_CONNECTION_POOL_OK
                         ? VPS_CLIENT_OK
                         : VPS_CLIENT_BACKEND_ERROR;
            if (status == VPS_CLIENT_OK)
                cursor->connection =
                    (VpsClientConnection *)cursor->lease.connection;
        }
    }
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = (const char *)cursor->planned_query.data;
    spec.query_length = cursor->planned_query.size;
    spec.parameters = cursor->parameters;
    spec.parameter_count = cursor->parameter_count;
    spec.result_fields = cursor->projected_fields;
    spec.result_field_count = cursor->remote_projection_count;
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
        (size_t)column_index >= cursor->table->visible_field_count +
            (cursor->table->has_hidden_identity ? 1U : 0U) +
            (cursor->table->mode_rw ? 1U : 0U))
        return SQLITE_MISUSE;
    if ((size_t)column_index == cursor->table->visible_field_count &&
        cursor->table->has_hidden_identity) {
        if (!cursor->initialized_identity_token) return SQLITE_CORRUPT_VTAB;
        sqlite3_result_blob64(context, cursor->identity_token.data,
                              (sqlite3_uint64)cursor->identity_token.size,
                              SQLITE_TRANSIENT);
        return SQLITE_OK;
    }
    if ((size_t)column_index >= cursor->table->visible_field_count) {
        sqlite3_result_null(context);
        return SQLITE_OK;
    }
    if (sqlite3_vtab_nochange(context)) return SQLITE_OK;
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

static int vps_dml_identity_from_sqlite(const VpsTable *table,
                                        sqlite3_value *value,
                                        char integer_text[32],
                                        VpsRowIdentityView *identity)
{
    if (table->identity_mode == VPS_ROW_IDENTITY_HIDDEN_TOKEN) {
        if (value == NULL || sqlite3_value_type(value) != SQLITE_BLOB)
            return 0;
        return vps_row_identity_decode(sqlite3_value_blob(value),
                                       (size_t)sqlite3_value_bytes(value),
                                       identity) == VPS_ROW_IDENTITY_OK;
    }
    if (value == NULL || sqlite3_value_type(value) != SQLITE_INTEGER ||
        table->key_column_count != 1U)
        return 0;
    {
        int length = snprintf(integer_text, 32U, "%lld",
                              (long long)sqlite3_value_int64(value));
        size_t logical = table->key_columns[0];
        if (length <= 0 || length >= 32) return 0;
        (void)memset(identity, 0, sizeof(*identity));
        identity->relation_oid = table->table_metadata.relation.relation_oid;
        identity->key_field_count = 1U;
        identity->key_fields[0].kind = VPS_ROW_IDENTITY_FIELD_TEXT;
        identity->key_fields[0].type_oid =
            table->dml_policy.selections[logical].declared_type_oid;
        identity->key_fields[0].bytes = integer_text;
        identity->key_fields[0].length = (size_t)length;
    }
    return 1;
}

static int vps_dml_find_visible(const VpsTable *table,
                                const unsigned char *name,
                                size_t length,
                                size_t *visible_out)
{
    size_t visible;
    for (visible = 0U; visible < table->visible_field_count; ++visible) {
        const VpsQueryDescribeField *field = &table->described.fields[visible];
        if (field->name_length == length &&
            memcmp(field->name, name, length) == 0) {
            *visible_out = visible;
            return 1;
        }
    }
    return 0;
}

static int vps_dml_insert_included(const VpsTable *table,
                                   sqlite3_value **arguments,
                                   unsigned char *included)
{
    size_t visible;
    size_t omit_index = 2U + table->visible_field_count +
                        (table->has_hidden_identity ? 1U : 0U);
    sqlite3_value *omit = arguments[omit_index];
    for (visible = 0U; visible < table->visible_field_count; ++visible)
        included[visible] = 1U;
    if (sqlite3_value_type(omit) == SQLITE_NULL) return 1;
    if (sqlite3_value_type(omit) != SQLITE_TEXT) return 0;
    {
        const unsigned char *text = sqlite3_value_text(omit);
        size_t length = (size_t)sqlite3_value_bytes(omit);
        size_t offset = 0U;
        if (length == 1U && text[0] == '*') {
            (void)memset(included, 0, table->visible_field_count);
            return 1;
        }
        while (offset < length) {
            size_t end = offset;
            size_t omitted;
            while (end < length && text[end] != ',') ++end;
            while (offset < end && (text[offset] == ' ' || text[offset] == '\t'))
                ++offset;
            while (end > offset && (text[end - 1U] == ' ' ||
                                    text[end - 1U] == '\t'))
                --end;
            if (end == offset ||
                !vps_dml_find_visible(table, text + offset, end - offset,
                                      &omitted))
                return 0;
            included[omitted] = 0U;
            offset = end;
            while (offset < length && text[offset] != ',') ++offset;
            if (offset < length) ++offset;
        }
    }
    return 1;
}

static int vps_dml_append_parameter(VpsBuffer *bytes,
                                    sqlite3_value *value,
                                    uint32_t type_oid,
                                    VpsClientParameterView *parameter,
                                    size_t *offset)
{
    char text[64];
    const void *source = NULL;
    size_t length = 0U;
    int sqlite_type = sqlite3_value_type(value);
    (void)memset(parameter, 0, sizeof(*parameter));
    parameter->type_oid = type_oid;
    parameter->format = type_oid == UINT32_C(17)
                            ? VPS_CLIENT_VALUE_BINARY
                            : VPS_CLIENT_VALUE_TEXT;
    *offset = bytes->size;
    if (sqlite_type == SQLITE_NULL) {
        parameter->is_null = 1;
        return 1;
    }
    if (type_oid == UINT32_C(17)) {
        if (sqlite_type != SQLITE_BLOB) return 0;
        source = sqlite3_value_blob(value);
        length = (size_t)sqlite3_value_bytes(value);
    } else if (type_oid == UINT32_C(16)) {
        if (sqlite_type != SQLITE_INTEGER) return 0;
        text[0] = sqlite3_value_int64(value) != 0 ? 't' : 'f';
        source = text;
        length = 1U;
    } else if (sqlite_type == SQLITE_INTEGER) {
        int written = snprintf(text, sizeof(text), "%lld",
                               (long long)sqlite3_value_int64(value));
        if (written <= 0 || (size_t)written >= sizeof(text)) return 0;
        source = text;
        length = (size_t)written;
    } else if (sqlite_type == SQLITE_FLOAT) {
        int written = snprintf(text, sizeof(text), "%.17g",
                               sqlite3_value_double(value));
        if (written <= 0 || (size_t)written >= sizeof(text)) return 0;
        source = text;
        length = (size_t)written;
    } else if (sqlite_type == SQLITE_TEXT) {
        source = sqlite3_value_text(value);
        length = (size_t)sqlite3_value_bytes(value);
    } else return 0;
    if (length != 0U &&
        vps_buffer_append(bytes, source, length) != VPS_MEMORY_OK)
        return 0;
    parameter->length = length;
    return parameter->format == VPS_CLIENT_VALUE_BINARY ||
           vps_buffer_append(bytes, "\0", 1U) == VPS_MEMORY_OK;
}

static int vps_dml_append_identity_parameter(
    VpsBuffer *bytes,
    const VpsRowIdentityField *field,
    uint32_t type_oid,
    VpsClientParameterView *parameter,
    size_t *offset)
{
    (void)memset(parameter, 0, sizeof(*parameter));
    parameter->type_oid = type_oid;
    parameter->format = type_oid == UINT32_C(17)
                            ? VPS_CLIENT_VALUE_BINARY
                            : VPS_CLIENT_VALUE_TEXT;
    parameter->is_null = field->kind == VPS_ROW_IDENTITY_FIELD_NULL;
    *offset = bytes->size;
    if (parameter->is_null) return 1;
    if (field->kind == VPS_ROW_IDENTITY_FIELD_INTEGER) {
        char text[32];
        int written = snprintf(text, sizeof(text), "%lld",
                               (long long)field->integer);
        if (written <= 0 || (size_t)written >= sizeof(text) ||
            vps_buffer_append(bytes, text, (size_t)written) != VPS_MEMORY_OK)
            return 0;
        parameter->length = (size_t)written;
        return vps_buffer_append(bytes, "\0", 1U) == VPS_MEMORY_OK;
    }
    if (field->length != 0U &&
        vps_buffer_append(bytes, field->bytes, field->length) != VPS_MEMORY_OK)
        return 0;
    parameter->length = field->length;
    return parameter->format == VPS_CLIENT_VALUE_BINARY ||
           vps_buffer_append(bytes, "\0", 1U) == VPS_MEMORY_OK;
}

static int vps_dml_append_srid_parameter(
    VpsBuffer *bytes, uint32_t srid, VpsClientParameterView *parameter,
    size_t *offset)
{
    char text[16];
    int written;
    if (srid > INT32_MAX) return 0;
    written = snprintf(text, sizeof(text), "%u", (unsigned int)srid);
    if (written <= 0 || (size_t)written >= sizeof(text)) return 0;
    (void)memset(parameter, 0, sizeof(*parameter));
    parameter->type_oid = UINT32_C(23);
    parameter->format = VPS_CLIENT_VALUE_TEXT;
    *offset = bytes->size;
    if (vps_buffer_append(bytes, text, (size_t)written) != VPS_MEMORY_OK ||
        vps_buffer_append(bytes, "\0", 1U) != VPS_MEMORY_OK)
        return 0;
    parameter->length = (size_t)written;
    return 1;
}

static VpsSpatialResult vps_dml_validate_spatial_value(
    const VpsTable *table, size_t visible, sqlite3_value *value)
{
    VpsSpatialLimits limits;
    VpsSpatialValidation validation;
    VpsSpatialResult spatial_result;
    int sqlite_type;
    if (table->spatial_kind[visible] == 0U ||
        table->spatial_format == VPS_SPATIAL_FORMAT_NONE)
        return VPS_SPATIAL_OK;
    sqlite_type = sqlite3_value_type(value);
    if (sqlite_type == SQLITE_NULL) return VPS_SPATIAL_OK;
    limits.max_bytes = VPS_SPATIAL_DEFAULT_MAX_BYTES;
    limits.max_points = VPS_SPATIAL_DEFAULT_MAX_POINTS;
    limits.max_depth = VPS_SPATIAL_DEFAULT_MAX_DEPTH;
    if (table->spatial_format == VPS_SPATIAL_FORMAT_WKT ||
        table->spatial_format == VPS_SPATIAL_FORMAT_EWKT) {
        if (sqlite_type != SQLITE_TEXT) return VPS_SPATIAL_INVALID_ARGUMENT;
        spatial_result = vps_spatial_validate_text(
            (const char *)sqlite3_value_text(value),
            (size_t)sqlite3_value_bytes(value), table->spatial_format,
            table->spatial_type[visible], table->spatial_srid[visible],
            &limits, &validation);
    } else {
        if (sqlite_type != SQLITE_BLOB) return VPS_SPATIAL_INVALID_ARGUMENT;
        spatial_result = vps_spatial_validate_binary(
            sqlite3_value_blob(value), (size_t)sqlite3_value_bytes(value),
            table->spatial_format, table->spatial_type[visible],
            table->spatial_srid[visible], &limits, &validation);
    }
    if (spatial_result != VPS_SPATIAL_OK) return spatial_result;
    return table->spatial_dimensions[visible] == 0U ||
                   table->spatial_dimensions[visible] == validation.dimensions
               ? VPS_SPATIAL_OK : VPS_SPATIAL_UNSUPPORTED;
}

static int vps_module_update(sqlite3_vtab *base,
                             int argument_count,
                             sqlite3_value **arguments,
                             sqlite3_int64 *rowid_out)
{
    VpsTable *table = (VpsTable *)base;
    VpsDmlOperation operation;
    VpsDmlPlan *plan = NULL;
    VpsRowIdentityView identity;
    VpsClientParameterView *parameters = NULL;
    VpsClientResultFieldExpectation *fields = NULL;
    size_t *offsets = NULL;
    unsigned char *included = NULL;
    size_t plan_bytes = sizeof(*plan);
    size_t parameter_array_bytes = sizeof(*parameters) * VPS_DML_MAX_PARAMETERS;
    size_t field_array_bytes =
        sizeof(*fields) * (VPS_METADATA_MAX_KEY_COLUMNS + 1U);
    size_t offset_array_bytes = sizeof(*offsets) * VPS_DML_MAX_PARAMETERS;
    size_t included_bytes = sizeof(*included) * VPS_DML_MAX_COLUMNS;
    VpsBuffer parameter_bytes;
    VpsConnectionLease lease;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    VpsClientStatementSpec spec;
    VpsClientStatementMetadata metadata;
    VpsError error;
    VpsClientStatus status = VPS_CLIENT_BACKEND_ERROR;
    VpsDmlResult dml_result;
    const VpsClientRowView *row = NULL;
    char integer_identity[32];
    char spatial_failure[64];
    size_t index;
    int error_initialized = 0;
    int plan_initialized = 0;
    int bytes_initialized = 0;
    int clean = 0;
    int in_transaction = 0;
    int result = SQLITE_ERROR;
    const char *failure = "dispatch";
    if (table == NULL || arguments == NULL || !table->mode_rw ||
        !table->initialized_dml_policy)
        return vps_vtab_set_error(base, SQLITE_READONLY, NULL,
                                  "VirtualPostgreSQL table is read-only");
    if (argument_count == 1)
        operation = VPS_DML_DELETE;
    else if ((size_t)argument_count ==
             2U + table->visible_field_count +
                 (table->has_hidden_identity ? 1U : 0U) + 1U)
        operation = sqlite3_value_type(arguments[0]) == SQLITE_NULL
                        ? VPS_DML_INSERT : VPS_DML_UPDATE;
    else
        return SQLITE_MISUSE;
    if (vps_memory_allocate(&table->allocator, plan_bytes,
                            (void **)&plan) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, parameter_array_bytes,
                            (void **)&parameters) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, field_array_bytes,
                            (void **)&fields) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, offset_array_bytes,
                            (void **)&offsets) != VPS_MEMORY_OK ||
        vps_memory_allocate(&table->allocator, included_bytes,
                            (void **)&included) != VPS_MEMORY_OK) {
        vps_memory_release(&table->allocator, (void **)&included,
                           included_bytes);
        vps_memory_release(&table->allocator, (void **)&offsets,
                           offset_array_bytes);
        vps_memory_release(&table->allocator, (void **)&fields,
                           field_array_bytes);
        vps_memory_release(&table->allocator, (void **)&parameters,
                           parameter_array_bytes);
        vps_memory_release(&table->allocator, (void **)&plan, plan_bytes);
        return SQLITE_NOMEM;
    }
    (void)memset(plan, 0, plan_bytes);
    (void)memset(&identity, 0, sizeof(identity));
    (void)memset(parameters, 0, parameter_array_bytes);
    (void)memset(fields, 0, field_array_bytes);
    (void)memset(included, 0, included_bytes);
    (void)memset(&lease, 0, sizeof(lease));
    (void)memset(&spec, 0, sizeof(spec));
    (void)memset(&metadata, 0, sizeof(metadata));
    (void)memset(&error, 0, sizeof(error));
    if (vps_error_init(&error, &table->allocator) == VPS_MEMORY_OK)
        error_initialized = 1;
    if (operation == VPS_DML_INSERT) {
        if (!vps_dml_insert_included(table, arguments, included)) {
            result = SQLITE_MISMATCH;
            goto cleanup;
        }
    } else if (operation == VPS_DML_UPDATE) {
        for (index = 0U; index < table->visible_field_count; ++index)
            included[index] =
                (unsigned char)!sqlite3_value_nochange(arguments[2U + index]);
        if (!vps_dml_identity_from_sqlite(table, arguments[0],
                                          integer_identity, &identity)) {
            result = SQLITE_CORRUPT_VTAB;
            goto cleanup;
        }
    } else if (!vps_dml_identity_from_sqlite(table, arguments[0],
                                              integer_identity, &identity)) {
        result = SQLITE_CORRUPT_VTAB;
        goto cleanup;
    }
    if (operation != VPS_DML_DELETE) {
        for (index = 0U; index < table->visible_field_count; ++index) {
            VpsSpatialResult spatial_result;
            if (!included[index]) continue;
            spatial_result = vps_dml_validate_spatial_value(
                table, index, arguments[2U + index]);
            if (spatial_result != VPS_SPATIAL_OK) {
                int written = snprintf(
                    spatial_failure, sizeof(spatial_failure), "%s column=%u",
                    vps_spatial_result_name(spatial_result),
                    (unsigned int)index);
                failure = written > 0 && (size_t)written < sizeof(spatial_failure)
                              ? spatial_failure : "spatial validation";
                result = SQLITE_MISMATCH;
                goto cleanup;
            }
        }
    }
    dml_result = vps_dml_plan_build(
        &table->allocator, &table->dml_policy, operation,
        operation == VPS_DML_DELETE ? NULL : included,
        operation == VPS_DML_DELETE ? 0U : table->visible_field_count,
        operation == VPS_DML_INSERT ? NULL : &identity, plan);
    if (dml_result != VPS_DML_OK) {
        failure = vps_dml_result_name(dml_result);
        result = dml_result == VPS_DML_GENERATED_COLUMN ||
                         dml_result == VPS_DML_IDENTITY_ALWAYS
                     ? SQLITE_CONSTRAINT
                     : SQLITE_ERROR;
        goto cleanup;
    }
    plan_initialized = 1;
    failure = "parameter encoding";
    if (vps_buffer_init(&parameter_bytes, &table->allocator,
                        VPS_VTAB_PARAMETER_BYTES_LIMIT) != VPS_MEMORY_OK) {
        result = SQLITE_NOMEM;
        goto cleanup;
    }
    bytes_initialized = 1;
    for (index = 0U; index < plan->parameter_count; ++index) {
        const VpsDmlParameterSlot *slot = &plan->parameters[index];
        int appended;
        if (slot->source == VPS_DML_PARAMETER_NEW_VALUE)
            appended = vps_dml_append_parameter(
                &parameter_bytes, arguments[2U + slot->index], slot->type_oid,
                &parameters[index], &offsets[index]);
        else if (slot->source == VPS_DML_PARAMETER_SPATIAL_SRID)
            appended = vps_dml_append_srid_parameter(
                &parameter_bytes, table->spatial_srid[slot->index],
                &parameters[index], &offsets[index]);
        else if (slot->source == VPS_DML_PARAMETER_OLD_KEY)
            appended = vps_dml_append_identity_parameter(
                &parameter_bytes, &identity.key_fields[slot->index],
                slot->type_oid, &parameters[index], &offsets[index]);
        else
            appended = vps_dml_append_identity_parameter(
                &parameter_bytes, &identity.optimistic_field,
                slot->type_oid, &parameters[index], &offsets[index]);
        if (!appended) {
            result = SQLITE_MISMATCH;
            goto cleanup;
        }
    }
    for (index = 0U; index < plan->parameter_count; ++index)
        if (!parameters[index].is_null)
            parameters[index].value = parameter_bytes.data + offsets[index];
    for (index = 0U; index < plan->returning_count; ++index) {
        if (plan->returning_visible[index] == UINT16_MAX)
            fields[index].type_oid = UINT32_C(25);
        else
            fields[index].type_oid = table->dml_policy.selections[
                plan->returning_visible[index]].declared_type_oid;
        fields[index].format = VPS_CLIENT_VALUE_TEXT;
    }
    {
        VpsTransactionStatus transaction_status;
        if (vps_transaction_status(&table->module_context->transaction,
                                   &transaction_status) == VPS_TRANSACTION_OK &&
            transaction_status.state != VPS_TRANSACTION_IDLE) {
            VpsTransactionResult transaction_result =
                vps_transaction_command_allowed(
                    &table->module_context->transaction);
            if (transaction_result != VPS_TRANSACTION_OK) {
                result = transaction_result == VPS_TRANSACTION_BUSY
                             ? SQLITE_BUSY : SQLITE_ABORT;
                failure = vps_transaction_result_name(transaction_result);
                goto cleanup;
            }
            connection = table->module_context->transaction_connection;
            if (connection == NULL) goto cleanup;
            in_transaction = 1;
        } else if (vps_connection_pool_acquire(
                       table->pool, &table->pool_key, 10000U,
                       NULL, NULL, &lease) != VPS_CONNECTION_POOL_OK) {
            goto cleanup;
        }
    }
    failure = "statement execution";
    if (!in_transaction)
        connection = (VpsClientConnection *)lease.connection;
    spec.query = (const char *)plan->query.data;
    spec.query_length = plan->query.size;
    spec.parameters = parameters;
    spec.parameter_count = plan->parameter_count;
    spec.result_fields = fields;
    spec.result_field_count = plan->returning_count;
    spec.timeout_ms = 60000U;
    spec.single_row = 1;
    spec.error_operation = VPS_ERROR_OPERATION_DML;
    status = vps_client_statement_open(connection, &spec, &statement,
                                       error_initialized ? &error : NULL);
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_start(statement,
                                            VPS_CLIENT_OPERATION_EXECUTE,
                                            error_initialized ? &error : NULL);
    if (status == VPS_CLIENT_OK)
        status = vps_drive_statement(statement, NULL,
                                     error_initialized ? &error : NULL);
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_start(statement,
                                            VPS_CLIENT_OPERATION_FETCH,
                                            error_initialized ? &error : NULL);
    if (status == VPS_CLIENT_OK)
        status = vps_drive_statement(statement, NULL,
                                     error_initialized ? &error : NULL);
    if (status == VPS_CLIENT_OK &&
        vps_client_statement_state(statement) == VPS_CLIENT_STATEMENT_COMPLETE) {
        failure = "zero-row affected count";
        if (vps_client_statement_metadata(
                statement, &metadata,
                error_initialized ? &error : NULL) != VPS_CLIENT_OK ||
            !metadata.affected_count_valid)
            goto cleanup;
        dml_result = vps_dml_classify_count(
            operation, table->optimistic_mode, metadata.affected_count);
        clean = 1;
        result = dml_result == VPS_DML_CONFLICT
                     ? SQLITE_BUSY
                     : dml_result == VPS_DML_NOT_FOUND
                           ? SQLITE_CONSTRAINT : SQLITE_ERROR;
        goto cleanup;
    }
    if (status != VPS_CLIENT_OK ||
        vps_client_statement_state(statement) != VPS_CLIENT_STATEMENT_ROW_READY ||
        vps_client_statement_current_row(statement, &row,
                                         error_initialized ? &error : NULL) !=
            VPS_CLIENT_OK ||
        vps_client_row_column_count(row) != plan->returning_count)
        goto cleanup;
    failure = "RETURNING row";
    if (operation == VPS_DML_INSERT && rowid_out != NULL &&
        table->identity_mode == VPS_ROW_IDENTITY_STABLE_INTEGER) {
        VpsClientColumnView key;
        int64_t rowid;
        if (vps_client_row_column(row, 0U, &key,
                                  error_initialized ? &error : NULL) !=
                VPS_CLIENT_OK ||
            vps_row_identity_stable_integer(&key, &rowid) !=
                VPS_ROW_IDENTITY_OK)
            goto cleanup;
        *rowid_out = (sqlite3_int64)rowid;
    }
    if (vps_client_statement_row_consumed(
            statement, error_initialized ? &error : NULL) != VPS_CLIENT_OK)
        goto cleanup;
    failure = "terminal fetch start";
    if (vps_client_statement_start(statement, VPS_CLIENT_OPERATION_FETCH,
                                   error_initialized ? &error : NULL) !=
            VPS_CLIENT_OK)
        goto cleanup;
    failure = "terminal fetch";
    if (vps_drive_statement(statement, NULL,
                            error_initialized ? &error : NULL) != VPS_CLIENT_OK ||
        vps_client_statement_state(statement) != VPS_CLIENT_STATEMENT_COMPLETE)
        goto cleanup;
    failure = "statement metadata";
    if (vps_client_statement_metadata(statement, &metadata,
                                      error_initialized ? &error : NULL) !=
            VPS_CLIENT_OK)
        goto cleanup;
    failure = "missing affected count";
    if (!metadata.affected_count_valid) goto cleanup;
    failure = "affected count";
    dml_result = vps_dml_classify_count(operation, table->optimistic_mode,
                                        metadata.affected_count);
    if (dml_result == VPS_DML_OK) {
        clean = 1;
        result = SQLITE_OK;
    } else if (dml_result == VPS_DML_CONFLICT) {
        clean = 1;
        result = SQLITE_BUSY;
    } else if (dml_result == VPS_DML_NOT_FOUND) {
        clean = 1;
        result = SQLITE_CONSTRAINT;
    }
cleanup:
    if (in_transaction && result != SQLITE_OK && status != VPS_CLIENT_OK)
        (void)vps_transaction_mark_failed(
            &table->module_context->transaction,
            error_initialized && error.sqlstate[0] != '\0'
                ? error.sqlstate : NULL);
    if (statement != NULL &&
        vps_client_statement_close(&statement) != VPS_CLIENT_OK)
        clean = 0;
    if (lease.pool != NULL)
        (void)vps_connection_lease_release(
            &lease, clean ? VPS_CONNECTION_LEASE_CLEAN
                          : VPS_CONNECTION_LEASE_DIRTY);
    if (bytes_initialized) vps_buffer_reset(&parameter_bytes);
    if (plan_initialized) vps_dml_plan_reset(plan);
    if (result != SQLITE_OK)
    {
        char fallback[128];
        if (error_initialized && error.sqlite_code != SQLITE_OK)
            result = error.sqlite_code;
        (void)snprintf(fallback, sizeof(fallback),
                       "VirtualPostgreSQL DML failed at %s", failure);
        result = vps_vtab_set_error(
            base, result,
            error_initialized && error.sqlite_code != SQLITE_OK ? &error : NULL,
            fallback);
    }
    if (error_initialized) vps_error_reset(&error);
    vps_memory_release(&table->allocator, (void **)&included, included_bytes);
    vps_memory_release(&table->allocator, (void **)&offsets,
                       offset_array_bytes);
    vps_memory_release(&table->allocator, (void **)&fields,
                       field_array_bytes);
    vps_memory_release(&table->allocator, (void **)&parameters,
                       parameter_array_bytes);
    vps_memory_release(&table->allocator, (void **)&plan, plan_bytes);
    return result;
}

static int vps_module_transaction_statement(VpsTable *table,
                                            const char *query,
                                            VpsErrorOperation operation,
                                            VpsError *error)
{
    VpsClientStatementSpec spec;
    VpsClientStatement *statement = NULL;
    VpsClientStatus status;
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = query;
    spec.query_length = strlen(query);
    spec.timeout_ms = 60000U;
    spec.error_operation = operation;
    status = vps_client_statement_open(
        table->module_context->transaction_connection, &spec, &statement,
        error);
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_start(statement,
                                            VPS_CLIENT_OPERATION_EXECUTE,
                                            error);
    if (status == VPS_CLIENT_OK)
        status = vps_drive_statement(statement, NULL, error);
    if (status == VPS_CLIENT_OK &&
        vps_client_statement_state(statement) !=
            VPS_CLIENT_STATEMENT_COMPLETE)
        status = vps_client_statement_start(statement,
                                            VPS_CLIENT_OPERATION_FETCH,
                                            error);
    if (status == VPS_CLIENT_OK &&
        vps_client_statement_state(statement) !=
            VPS_CLIENT_STATEMENT_COMPLETE)
        status = vps_drive_statement(statement, NULL, error);
    if (status == VPS_CLIENT_OK &&
        vps_client_statement_state(statement) !=
            VPS_CLIENT_STATEMENT_COMPLETE)
        status = VPS_CLIENT_BACKEND_ERROR;
    if (statement != NULL &&
        vps_client_statement_close(&statement) != VPS_CLIENT_OK)
        status = VPS_CLIENT_BACKEND_ERROR;
    return status == VPS_CLIENT_OK ? SQLITE_OK : SQLITE_ERROR;
}

static int vps_module_begin(sqlite3_vtab *base)
{
    VpsTable *table = (VpsTable *)base;
    VpsModuleContext *context;
    VpsTransactionResult transaction_result;
    VpsError error;
    VpsClientStatus status = VPS_CLIENT_BACKEND_ERROR;
    int error_initialized = 0;
    if (table == NULL || !table->mode_rw || table->module_context == NULL)
        return SQLITE_READONLY;
    context = table->module_context;
    context->transaction.logger = &table->logger;
    transaction_result = vps_transaction_begin(
        &context->transaction, &table->identity, table->table_id);
    if (transaction_result == VPS_TRANSACTION_OK) return SQLITE_OK;
    if (transaction_result != VPS_TRANSACTION_COMMAND_REQUIRED)
        return vps_vtab_set_error(
            base,
            transaction_result == VPS_TRANSACTION_BUSY ? SQLITE_BUSY
                : transaction_result == VPS_TRANSACTION_IDENTITY_MISMATCH
                      ? SQLITE_CONSTRAINT : SQLITE_ABORT,
            NULL, "VirtualPostgreSQL transaction participant rejected");
    (void)memset(&context->transaction_lease, 0,
                 sizeof(context->transaction_lease));
    (void)memset(&error, 0, sizeof(error));
    if (vps_error_init(&error, &table->allocator) == VPS_MEMORY_OK)
        error_initialized = 1;
    if (vps_connection_pool_acquire(
            table->pool, &table->pool_key, 10000U, NULL, NULL,
            &context->transaction_lease) == VPS_CONNECTION_POOL_OK) {
        context->transaction_connection =
            (VpsClientConnection *)context->transaction_lease.connection;
        status = vps_client_connection_start(
            context->transaction_connection, VPS_CLIENT_OPERATION_BEGIN,
            error_initialized ? &error : NULL);
        if (status == VPS_CLIENT_OK)
            status = vps_drive_connection(
                context->transaction_connection,
                error_initialized ? &error : NULL);
    }
    if (status == VPS_CLIENT_OK) {
        const char *isolation =
            table->transaction_isolation ==
                    VPS_ARGUMENT_ENUM_ISOLATION_REPEATABLE_READ
                ? "REPEATABLE READ"
                : table->transaction_isolation ==
                          VPS_ARGUMENT_ENUM_ISOLATION_SERIALIZABLE
                      ? "SERIALIZABLE" : "READ COMMITTED";
        char options_query[128];
        int written = snprintf(
            options_query, sizeof(options_query),
            "SET TRANSACTION ISOLATION LEVEL %s %s", isolation,
            table->transaction_read_only ? "READ ONLY" : "READ WRITE");
        if (written <= 0 || (size_t)written >= sizeof(options_query) ||
            vps_module_transaction_statement(
                table, options_query, VPS_ERROR_OPERATION_QUERY,
                error_initialized ? &error : NULL) != SQLITE_OK)
            status = VPS_CLIENT_BACKEND_ERROR;
    }
    if (status == VPS_CLIENT_OK) {
        (void)vps_transaction_begin_complete(&context->transaction, 1);
        if (error_initialized) vps_error_reset(&error);
        return SQLITE_OK;
    }
    (void)vps_transaction_begin_complete(&context->transaction, 0);
    if (context->transaction_lease.pool != NULL)
        (void)vps_connection_lease_release(
            &context->transaction_lease, VPS_CONNECTION_LEASE_DIRTY);
    context->transaction_connection = NULL;
    {
        int sqlite_result = vps_vtab_set_error(
            base, SQLITE_ERROR, error_initialized ? &error : NULL,
            "VirtualPostgreSQL BEGIN failed");
        if (error_initialized) vps_error_reset(&error);
        return sqlite_result;
    }
}

static int vps_module_sync(sqlite3_vtab *base)
{
    VpsTable *table = (VpsTable *)base;
    VpsTransactionStatus status;
    VpsTransactionResult result;
    if (table == NULL || table->module_context == NULL) return SQLITE_MISUSE;
    if (vps_transaction_status(&table->module_context->transaction, &status) !=
            VPS_TRANSACTION_OK || status.state == VPS_TRANSACTION_IDLE)
        return SQLITE_OK;
    result = vps_transaction_command_allowed(
        &table->module_context->transaction);
    return result == VPS_TRANSACTION_OK ? SQLITE_OK
           : result == VPS_TRANSACTION_BUSY ? SQLITE_BUSY
                                            : SQLITE_ABORT;
}

static int vps_module_end(sqlite3_vtab *base,
                          VpsTransactionEndOperation operation)
{
    VpsTable *table = (VpsTable *)base;
    VpsModuleContext *context;
    VpsTransactionResult transaction_result;
    VpsClientOperation client_operation;
    VpsError error;
    VpsClientStatus status;
    int connection_lost;
    int result;
    if (table == NULL || table->module_context == NULL) return SQLITE_MISUSE;
    context = table->module_context;
    transaction_result = vps_transaction_end(&context->transaction, operation);
    if (transaction_result == VPS_TRANSACTION_OK) return SQLITE_OK;
    if (transaction_result != VPS_TRANSACTION_COMMAND_REQUIRED)
        return transaction_result == VPS_TRANSACTION_BUSY ? SQLITE_BUSY
                                                           : SQLITE_ABORT;
    if (vps_error_init(&error, &table->allocator) != VPS_MEMORY_OK)
        return SQLITE_NOMEM;
    client_operation = operation == VPS_TRANSACTION_END_COMMIT
                           ? VPS_CLIENT_OPERATION_COMMIT
                           : VPS_CLIENT_OPERATION_ROLLBACK;
    status = context->transaction_connection != NULL
                 ? vps_client_connection_start(context->transaction_connection,
                                               client_operation, &error)
                 : VPS_CLIENT_INVALID_STATE;
    if (status == VPS_CLIENT_OK)
        status = vps_drive_connection(context->transaction_connection, &error);
    connection_lost = status != VPS_CLIENT_OK &&
        (error.error_class == VPS_ERROR_CLASS_CONNECTION ||
         error.error_class == VPS_ERROR_CLASS_TIMEOUT ||
         error.error_class == VPS_ERROR_CLASS_CANCEL);
    if (connection_lost) error.ambiguous = 1;
    transaction_result = vps_transaction_end_complete(
        &context->transaction, status == VPS_CLIENT_OK, connection_lost);
    if (context->transaction_lease.pool != NULL)
        (void)vps_connection_lease_release(
            &context->transaction_lease,
            status == VPS_CLIENT_OK ? VPS_CONNECTION_LEASE_CLEAN
                                    : VPS_CONNECTION_LEASE_DIRTY);
    context->transaction_connection = NULL;
    result = status == VPS_CLIENT_OK && transaction_result == VPS_TRANSACTION_OK
                 ? SQLITE_OK
                 : vps_vtab_set_error(
                       base, SQLITE_ERROR, &error,
                       connection_lost
                           ? "VirtualPostgreSQL transaction outcome is ambiguous"
                           : "VirtualPostgreSQL transaction end failed");
    vps_error_reset(&error);
    return result;
}

static int vps_module_commit(sqlite3_vtab *base)
{
    return vps_module_end(base, VPS_TRANSACTION_END_COMMIT);
}

static int vps_module_rollback(sqlite3_vtab *base)
{
    return vps_module_end(base, VPS_TRANSACTION_END_ROLLBACK);
}

static int vps_module_savepoint_action(sqlite3_vtab *base,
                                       int level,
                                       int action)
{
    VpsTable *table = (VpsTable *)base;
    VpsTransactionResult transaction_result;
    VpsError error;
    char query[96];
    int written;
    int result;
    if (table == NULL || table->module_context == NULL || level < 0)
        return SQLITE_MISUSE;
    if (action == 0)
        transaction_result = vps_transaction_savepoint(
            &table->module_context->transaction, level);
    else if (action == 1)
        transaction_result = vps_transaction_release(
            &table->module_context->transaction, level);
    else
        transaction_result = vps_transaction_rollback_to(
            &table->module_context->transaction, level);
    if (transaction_result == VPS_TRANSACTION_OK) return SQLITE_OK;
    if (transaction_result != VPS_TRANSACTION_COMMAND_REQUIRED)
        return transaction_result == VPS_TRANSACTION_BUSY ? SQLITE_BUSY
                                                           : SQLITE_ABORT;
    written = snprintf(query, sizeof(query),
                       action == 0 ? "SAVEPOINT vps_%d"
                       : action == 1 ? "RELEASE SAVEPOINT vps_%d"
                                     : "ROLLBACK TO SAVEPOINT vps_%d",
                       level);
    if (written <= 0 || (size_t)written >= sizeof(query)) return SQLITE_TOOBIG;
    if (vps_error_init(&error, &table->allocator) != VPS_MEMORY_OK)
        return SQLITE_NOMEM;
    result = vps_module_transaction_statement(
        table, query,
        action == 2 ? VPS_ERROR_OPERATION_ROLLBACK
                    : VPS_ERROR_OPERATION_QUERY,
        &error);
    if (result != SQLITE_OK) {
        (void)vps_transaction_mark_failed(
            &table->module_context->transaction,
            error.sqlstate[0] != '\0' ? error.sqlstate : NULL);
        result = vps_vtab_set_error(base, SQLITE_ERROR, &error,
                                    "VirtualPostgreSQL savepoint failed");
    }
    vps_error_reset(&error);
    return result;
}

static int vps_module_savepoint(sqlite3_vtab *base, int level)
{
    return vps_module_savepoint_action(base, level, 0);
}

static int vps_module_release(sqlite3_vtab *base, int level)
{
    return vps_module_savepoint_action(base, level, 1);
}

static int vps_module_rollback_to(sqlite3_vtab *base, int level)
{
    return vps_module_savepoint_action(base, level, 2);
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
    VpsTable *vtab = (VpsTable *)table;
    VpsMetadataSnapshot snapshot = {0};
    int result;
    (void)flags;
    if (error_out != NULL) *error_out = NULL;
    if (vtab == NULL || schema == NULL || table_name == NULL)
        return SQLITE_MISUSE;
    if (strcmp(schema, vtab->shadow_schema) != 0 ||
        strcmp(table_name, vtab->shadow_name) != 0) {
        if (error_out != NULL) {
            *error_out = sqlite3_mprintf(
                "VirtualPostgreSQL integrity target mismatch");
            if (*error_out == NULL) return SQLITE_NOMEM;
        }
        return SQLITE_OK;
    }
    result = vps_shadow_load(vtab, &snapshot);
    vps_metadata_snapshot_reset(&snapshot);
    if (result == SQLITE_NOMEM) return SQLITE_NOMEM;
    if (result != SQLITE_OK && error_out != NULL) {
        *error_out = sqlite3_mprintf(
            "VirtualPostgreSQL shadow metadata is inconsistent");
        if (*error_out == NULL) return SQLITE_NOMEM;
    }
    return SQLITE_OK;
}

const sqlite3_module VPS_MODULE = {
    .iVersion = 4,
    .xCreate = vps_module_create,
    .xConnect = vps_module_connect,
    .xBestIndex = vps_module_best_index,
    .xDisconnect = vps_module_disconnect,
    .xDestroy = vps_module_destroy,
    .xOpen = vps_module_open,
    .xClose = vps_module_close,
    .xFilter = vps_module_filter,
    .xNext = vps_module_next,
    .xEof = vps_module_eof,
    .xColumn = vps_module_column,
    .xRowid = vps_module_rowid,
    .xUpdate = vps_module_update,
    .xBegin = vps_module_begin,
    .xSync = vps_module_sync,
    .xCommit = vps_module_commit,
    .xRollback = vps_module_rollback,
    .xSavepoint = vps_module_savepoint,
    .xRelease = vps_module_release,
    .xRollbackTo = vps_module_rollback_to,
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
    static const char relations_decl[] =
        "CREATE TABLE x(schema_name TEXT,relation_name TEXT,relation_oid INTEGER,relkind TEXT,persistence TEXT,owner TEXT,tablespace TEXT,estimated_rows REAL,estimated_pages INTEGER,relation_bytes INTEGER,total_bytes INTEGER,comment TEXT,row_security INTEGER,force_row_security INTEGER,is_partition INTEGER,parent_schema TEXT,parent_relation TEXT,parent_oid INTEGER,readable INTEGER,writable_candidate INTEGER,supported INTEGER,statistics_available INTEGER,classification TEXT,connection TEXT HIDDEN,schema TEXT HIDDEN)";
    static const char table_info_decl[] =
        "CREATE TABLE x(cid INTEGER,name TEXT,type TEXT,\"notnull\" INTEGER,dflt_value TEXT,pk INTEGER,hidden INTEGER,attnum INTEGER,type_oid INTEGER,type_modifier INTEGER,type_schema TEXT,type_name TEXT,domain_schema TEXT,domain_name TEXT,array_element_schema TEXT,array_element_name TEXT,collation TEXT,identity_kind TEXT,generated_kind TEXT,storage_kind TEXT,compression_kind TEXT,comment TEXT,postgis_kind TEXT,postgis_srid INTEGER,postgis_typmod INTEGER,connection TEXT HIDDEN,schema TEXT HIDDEN,relation TEXT HIDDEN)";
    static const char index_list_decl[] =
        "CREATE TABLE x(seq INTEGER,name TEXT,unique_flag INTEGER,origin TEXT,partial INTEGER,valid INTEGER,ready INTEGER,immediate INTEGER,primary_flag INTEGER,exclusion INTEGER,nulls_not_distinct INTEGER,access_method TEXT,key_columns INTEGER,include_columns INTEGER,predicate TEXT,has_expressions INTEGER,connection TEXT HIDDEN,schema TEXT HIDDEN,relation TEXT HIDDEN)";
    static const char index_info_decl[] =
        "CREATE TABLE x(seqno INTEGER,cid INTEGER,name TEXT,descending INTEGER,nulls_first INTEGER,collation TEXT,opclass TEXT,included INTEGER,expression TEXT,key_column INTEGER,type_oid INTEGER,type_modifier INTEGER,ordinal INTEGER,connection TEXT HIDDEN,schema TEXT HIDDEN,relation TEXT HIDDEN,index_name TEXT HIDDEN)";
    static const char type_info_decl[] =
        "CREATE TABLE x(type_schema TEXT,type_name TEXT,type_oid INTEGER,type_kind TEXT,type_category TEXT,type_length INTEGER,by_value INTEGER,alignment TEXT,storage TEXT,not_null INTEGER,base_type_oid INTEGER,base_type_modifier INTEGER,element_type_oid INTEGER,array_type_oid INTEGER,collation TEXT,extension_name TEXT,extension_version TEXT,formatted_type TEXT,connection TEXT HIDDEN,requested_schema TEXT HIDDEN,requested_name TEXT HIDDEN)";
    static const char extensions_decl[] =
        "CREATE TABLE x(name TEXT,version TEXT,schema_name TEXT,schema_oid INTEGER,relocatable INTEGER,config TEXT,condition TEXT,extension_oid INTEGER,connection TEXT HIDDEN)";
    const char *declaration = NULL;
    VpsMetadataTable *table;
    int result;
    (void)error_out;
    if (database == NULL || table_out == NULL || argument_count != 3)
        return SQLITE_MISUSE;
    if (strcmp(arguments[0], "virtualpostgresql_relations") == 0) {
        declaration = relations_decl;
    } else if (strcmp(arguments[0], "virtualpostgresql_table_info") == 0) {
        declaration = table_info_decl;
    } else if (strcmp(arguments[0], "virtualpostgresql_index_list") == 0) {
        declaration = index_list_decl;
    } else if (strcmp(arguments[0], "virtualpostgresql_index_info") == 0) {
        declaration = index_info_decl;
    } else if (strcmp(arguments[0], "virtualpostgresql_type_info") == 0) {
        declaration = type_info_decl;
    } else if (strcmp(arguments[0], "virtualpostgresql_extensions") == 0) {
        declaration = extensions_decl;
    } else return SQLITE_MISUSE;
    result = sqlite3_declare_vtab(database, declaration);
    if (result != SQLITE_OK) return result;
    result = sqlite3_vtab_config(database, SQLITE_VTAB_DIRECTONLY);
    if (result != SQLITE_OK) return result;
    table = (VpsMetadataTable *)sqlite3_malloc64(sizeof(*table));
    if (table == NULL) return SQLITE_NOMEM;
    (void)memset(table, 0, sizeof(*table));
    table->database = database;
    table->module_context = (VpsModuleContext *)auxiliary;
    if (strcmp(arguments[0], "virtualpostgresql_relations") == 0) {
        table->kind = VPS_METADATA_FUNCTION_RELATIONS;
        table->query = VPS_CATALOG_QUERY_RELATIONS_FUNCTION;
        table->visible_count = 23U; table->hidden_count = 2U;
    } else if (strcmp(arguments[0], "virtualpostgresql_table_info") == 0) {
        table->kind = VPS_METADATA_FUNCTION_TABLE_INFO;
        table->query = VPS_CATALOG_QUERY_TABLE_INFO_FUNCTION;
        table->visible_count = 25U; table->hidden_count = 3U;
    } else if (strcmp(arguments[0], "virtualpostgresql_index_list") == 0) {
        table->kind = VPS_METADATA_FUNCTION_INDEX_LIST;
        table->query = VPS_CATALOG_QUERY_INDEX_LIST_FUNCTION;
        table->visible_count = 16U; table->hidden_count = 3U;
    } else if (strcmp(arguments[0], "virtualpostgresql_index_info") == 0) {
        table->kind = VPS_METADATA_FUNCTION_INDEX_INFO;
        table->query = VPS_CATALOG_QUERY_INDEX_INFO_FUNCTION;
        table->visible_count = 13U; table->hidden_count = 4U;
    } else if (strcmp(arguments[0], "virtualpostgresql_type_info") == 0) {
        table->kind = VPS_METADATA_FUNCTION_TYPE_INFO;
        table->query = VPS_CATALOG_QUERY_TYPE_INFO_FUNCTION;
        table->visible_count = 18U; table->hidden_count = 3U;
    } else {
        table->kind = VPS_METADATA_FUNCTION_EXTENSIONS;
        table->query = VPS_CATALOG_QUERY_EXTENSIONS_FUNCTION;
        table->visible_count = 8U; table->hidden_count = 1U;
    }
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
    VpsMetadataCursor *cursor;
    if (cursor_out == NULL) return SQLITE_MISUSE;
    cursor = (VpsMetadataCursor *)sqlite3_malloc64(sizeof(*cursor));
    if (cursor == NULL) return SQLITE_NOMEM;
    (void)memset(cursor, 0, sizeof(*cursor));
    cursor->table = (VpsMetadataTable *)table;
    *cursor_out = &cursor->base;
    return SQLITE_OK;
}

static int vps_metadata_close(sqlite3_vtab_cursor *cursor)
{
    VpsMetadataCursor *metadata = (VpsMetadataCursor *)cursor;
    if (metadata == NULL) return SQLITE_OK;
    if (metadata->lease.pool != NULL)
        (void)vps_connection_lease_release(&metadata->lease,
                                           VPS_CONNECTION_LEASE_CLEAN);
    if (metadata->initialized_rows) vps_metadata_rowset_reset(&metadata->rows);
    if (metadata->runtime != NULL) (void)vps_table_cleanup(metadata->runtime);
    sqlite3_free(cursor);
    return SQLITE_OK;
}

static int vps_metadata_runtime_create(VpsMetadataCursor *cursor,
                                       const unsigned char *connection,
                                       size_t connection_length)
{
    const char *argv[7] = {"VirtualPostgreSQL", "main", "metadata", NULL,
                           "source='table'", "schema='pg_catalog'",
                           "table='pg_class'"};
    char *connarg;
    size_t index;
    size_t out = 0U;
    size_t allocation;
    VpsError ignored;
    if (cursor == NULL || connection == NULL || connection_length == 0U ||
        connection_length > 4096U ||
        vps_size_multiply(connection_length, 2U, &allocation) !=
            VPS_MEMORY_OK || allocation > SIZE_MAX - 12U)
        return SQLITE_MISUSE;
    allocation += 12U;
    connarg = (char *)sqlite3_malloc64((sqlite3_uint64)allocation);
    cursor->runtime = (VpsTable *)sqlite3_malloc64(sizeof(*cursor->runtime));
    if (connarg == NULL || cursor->runtime == NULL) {
        sqlite3_free(connarg);
        sqlite3_free(cursor->runtime); cursor->runtime = NULL;
        return SQLITE_NOMEM;
    }
    (void)memset(cursor->runtime, 0, sizeof(*cursor->runtime));
    (void)memset(&ignored, 0, sizeof(ignored));
    (void)memcpy(connarg, "connstr='", 9U); out = 9U;
    for (index = 0U; index < connection_length; ++index) {
        if (connection[index] == '\0') {
            (void)vps_platform_secure_zero(vps_platform_current_operations(),
                                           connarg, allocation);
            sqlite3_free(connarg);
            (void)vps_table_cleanup(cursor->runtime); cursor->runtime = NULL;
            return SQLITE_MISUSE;
        }
        connarg[out++] = (char)connection[index];
        if (connection[index] == '\'') connarg[out++] = '\'';
    }
    connarg[out++] = '\''; connarg[out] = '\0';
    argv[3] = connarg;
    index = (size_t)vps_table_initialize_runtime(cursor->runtime, 7, argv,
                                                 &ignored);
    (void)vps_platform_secure_zero(vps_platform_current_operations(), connarg,
                                   allocation);
    sqlite3_free(connarg);
    return index == SQLITE_OK ? SQLITE_OK : (int)index;
}

static int vps_metadata_filter(sqlite3_vtab_cursor *cursor,
                               int index_number,
                               const char *index_string,
                               int argument_count,
                               sqlite3_value **arguments)
{
    VpsMetadataCursor *metadata = (VpsMetadataCursor *)cursor;
    VpsClientParameterView parameters[3];
    VpsError error;
    int result = SQLITE_OK;
    size_t parameter;
    (void)index_string;
    if (metadata == NULL || metadata->table == NULL || arguments == NULL ||
        index_number != (int)((1U << metadata->table->hidden_count) - 1U) ||
        argument_count != (int)metadata->table->hidden_count)
        return SQLITE_CONSTRAINT;
    if (metadata->lease.pool != NULL)
        (void)vps_connection_lease_release(&metadata->lease,
                                           VPS_CONNECTION_LEASE_CLEAN);
    if (metadata->initialized_rows) {
        vps_metadata_rowset_reset(&metadata->rows);
        metadata->initialized_rows = 0;
    }
    if (metadata->runtime != NULL) {
        (void)vps_table_cleanup(metadata->runtime);
        metadata->runtime = NULL;
    }
    metadata->row = 0U;
    if (sqlite3_value_type(arguments[0]) == SQLITE_NULL) return SQLITE_MISUSE;
    result = vps_metadata_runtime_create(
        metadata, sqlite3_value_text(arguments[0]),
        (size_t)sqlite3_value_bytes(arguments[0]));
    if (result != SQLITE_OK) goto fail;
    if (metadata->runtime == NULL) {
        result = SQLITE_INTERNAL;
        goto fail;
    }
    if (vps_connection_pool_acquire(
            metadata->runtime->pool, &metadata->runtime->pool_key, 10000U,
            NULL, NULL, &metadata->lease) != VPS_CONNECTION_POOL_OK) {
        result = SQLITE_CANTOPEN;
        goto fail;
    }
    if (vps_metadata_rowset_init(&metadata->rows,
                                 &metadata->runtime->allocator,
                                 &metadata->runtime->logger) != VPS_METADATA_OK) {
        result = SQLITE_NOMEM;
        goto fail;
    }
    metadata->initialized_rows = 1;
    (void)memset(parameters, 0, sizeof(parameters));
    for (parameter = 1U; parameter < metadata->table->hidden_count;
         ++parameter) {
        if (sqlite3_value_type(arguments[parameter]) == SQLITE_NULL) {
            result = SQLITE_MISUSE;
            goto fail;
        }
        parameters[parameter - 1U].value =
            sqlite3_value_text(arguments[parameter]);
        parameters[parameter - 1U].length =
            (size_t)sqlite3_value_bytes(arguments[parameter]);
        parameters[parameter - 1U].type_oid = VPS_METADATA_TEXT_OID;
        parameters[parameter - 1U].format = VPS_CLIENT_VALUE_TEXT;
    }
    if (vps_error_init(&error, &metadata->runtime->allocator) != VPS_MEMORY_OK) {
        result = SQLITE_NOMEM;
        goto fail;
    }
    if (vps_catalog_metadata_fetch(
            (VpsClientConnection *)metadata->lease.connection,
            metadata->table->query, parameters,
            metadata->table->hidden_count - 1U, &metadata->rows,
            &error) != VPS_CLIENT_OK) {
        result = error.sqlite_code != SQLITE_OK ? error.sqlite_code
                                                 : SQLITE_ERROR;
        if (vps_error_message(&error) != NULL)
            metadata->table->base.zErrMsg = sqlite3_mprintf(
                "%s", vps_error_message(&error));
    }
    vps_error_reset(&error);
    if (result == SQLITE_OK) {
        (void)vps_connection_lease_release(&metadata->lease,
                                           VPS_CONNECTION_LEASE_CLEAN);
        return SQLITE_OK;
    }
fail:
    if (metadata->lease.pool != NULL)
        (void)vps_connection_lease_release(&metadata->lease,
                                           VPS_CONNECTION_LEASE_DIRTY);
    return result;
}

static int vps_metadata_best_index(sqlite3_vtab *table,
                                   sqlite3_index_info *index_info)
{
    VpsMetadataTable *metadata = (VpsMetadataTable *)table;
    size_t hidden;
    int argv_index = 1;
    int mask = 0;
    if (index_info == NULL) return SQLITE_MISUSE;
    for (hidden = 0U; hidden < metadata->hidden_count; ++hidden) {
        int column = (int)(metadata->visible_count + hidden);
        int constraint;
        for (constraint = 0; constraint < index_info->nConstraint;
             ++constraint) {
            if (index_info->aConstraint[constraint].iColumn == column &&
                index_info->aConstraint[constraint].op ==
                    SQLITE_INDEX_CONSTRAINT_EQ &&
                index_info->aConstraint[constraint].usable) {
                index_info->aConstraintUsage[constraint].argvIndex = argv_index++;
                index_info->aConstraintUsage[constraint].omit = 1;
                mask |= 1 << hidden;
                break;
            }
        }
    }
    index_info->idxNum = mask;
    index_info->estimatedCost =
        mask == (int)((1U << metadata->hidden_count) - 1U) ? 100.0 : 1.0e99;
    index_info->estimatedRows = mask ==
        (int)((1U << metadata->hidden_count) - 1U) ? 100 : 65536;
    return SQLITE_OK;
}

static int vps_metadata_next(sqlite3_vtab_cursor *cursor)
{
    VpsMetadataCursor *metadata = (VpsMetadataCursor *)cursor;
    if (metadata == NULL) return SQLITE_MISUSE;
    if (metadata->row < metadata->rows.row_count) ++metadata->row;
    return SQLITE_OK;
}

static int vps_metadata_eof(sqlite3_vtab_cursor *cursor)
{
    VpsMetadataCursor *metadata = (VpsMetadataCursor *)cursor;
    return metadata == NULL || !metadata->initialized_rows ||
           metadata->row >= metadata->rows.row_count;
}

static int vps_metadata_integer_column(VpsMetadataFunctionKind kind,
                                       size_t column)
{
    switch (kind) {
        case VPS_METADATA_FUNCTION_RELATIONS:
            return column == 2U || (column >= 8U && column <= 10U) ||
                   (column >= 12U && column <= 14U) ||
                   (column >= 17U && column <= 21U);
        case VPS_METADATA_FUNCTION_TABLE_INFO:
            return column == 0U || column == 3U || column == 5U ||
                   column == 6U || column == 7U || column == 8U ||
                   column == 9U || column == 23U || column == 24U;
        case VPS_METADATA_FUNCTION_INDEX_LIST:
            return column == 0U || column == 2U ||
                   (column >= 4U && column <= 10U) ||
                   column == 12U || column == 13U || column == 15U;
        case VPS_METADATA_FUNCTION_INDEX_INFO:
            return column == 0U || column == 1U || column == 3U ||
                   column == 4U || column == 7U ||
                   (column >= 9U && column <= 12U);
        case VPS_METADATA_FUNCTION_TYPE_INFO:
            return column == 2U || column == 5U || column == 6U ||
                   (column >= 9U && column <= 13U);
        case VPS_METADATA_FUNCTION_EXTENSIONS:
            return column == 3U || column == 4U || column == 7U;
        default: return 0;
    }
}

static int vps_metadata_parse_int64(const unsigned char *value,
                                    size_t length,
                                    sqlite3_int64 *parsed)
{
    uint64_t magnitude = 0U;
    size_t index = 0U;
    int negative = 0;
    if (value == NULL || length == 0U || parsed == NULL) return 0;
    if ((length == 1U && value[0] == 't') ||
        (length == 4U && memcmp(value, "true", 4U) == 0)) {
        *parsed = 1; return 1;
    }
    if ((length == 1U && value[0] == 'f') ||
        (length == 5U && memcmp(value, "false", 5U) == 0)) {
        *parsed = 0; return 1;
    }
    if (value[0] == '-') { negative = 1; index = 1U; }
    if (index == length) return 0;
    for (; index < length; ++index) {
        uint64_t digit;
        if (value[index] < '0' || value[index] > '9') return 0;
        digit = (uint64_t)(value[index] - '0');
        if (magnitude > (UINT64_MAX - digit) / 10U) return 0;
        magnitude = magnitude * 10U + digit;
    }
    if ((!negative && magnitude > (uint64_t)INT64_MAX) ||
        (negative && magnitude > (uint64_t)INT64_MAX + 1U)) return 0;
    *parsed = negative
        ? (magnitude == (uint64_t)INT64_MAX + 1U ? INT64_MIN
                                                : -(sqlite3_int64)magnitude)
        : (sqlite3_int64)magnitude;
    return 1;
}

static int vps_metadata_column(sqlite3_vtab_cursor *cursor,
                               sqlite3_context *context,
                               int column)
{
    VpsMetadataCursor *metadata = (VpsMetadataCursor *)cursor;
    const unsigned char *value = NULL;
    size_t length = 0U;
    int is_null = 1;
    if (metadata == NULL || context == NULL || column < 0 ||
        (size_t)column >= metadata->table->visible_count ||
        vps_metadata_rowset_cell(&metadata->rows, metadata->row,
                                 (size_t)column, &value, &length,
                                 &is_null) != VPS_METADATA_OK)
        return SQLITE_RANGE;
    if (is_null) sqlite3_result_null(context);
    else if (length > INT_MAX) return SQLITE_TOOBIG;
    else if (vps_metadata_integer_column(metadata->table->kind,
                                          (size_t)column)) {
        sqlite3_int64 integer;
        if (!vps_metadata_parse_int64(value, length, &integer))
            return SQLITE_CORRUPT;
        sqlite3_result_int64(context, integer);
    } else if (metadata->table->kind == VPS_METADATA_FUNCTION_RELATIONS &&
               column == 7) {
        char number[64];
        char *end = NULL;
        double real;
        if (length == 0U || length >= sizeof(number)) return SQLITE_CORRUPT;
        (void)memcpy(number, value, length); number[length] = '\0';
        real = strtod(number, &end);
        if (end == NULL || *end != '\0') return SQLITE_CORRUPT;
        sqlite3_result_double(context, real);
    } else sqlite3_result_text(context, (const char *)value, (int)length,
                               SQLITE_TRANSIENT);
    return SQLITE_OK;
}

static int vps_metadata_rowid(sqlite3_vtab_cursor *cursor,
                              sqlite3_int64 *rowid)
{
    VpsMetadataCursor *metadata = (VpsMetadataCursor *)cursor;
    if (rowid == NULL) return SQLITE_MISUSE;
    *rowid = (sqlite3_int64)metadata->row + 1;
    return SQLITE_OK;
}

const sqlite3_module VPS_METADATA_MODULE = {
    .iVersion = 3,
    .xConnect = vps_metadata_connect,
    .xBestIndex = vps_metadata_best_index,
    .xDisconnect = vps_metadata_disconnect,
    .xOpen = vps_metadata_open,
    .xClose = vps_metadata_close,
    .xFilter = vps_metadata_filter,
    .xNext = vps_metadata_next,
    .xEof = vps_metadata_eof,
    .xColumn = vps_metadata_column,
    .xRowid = vps_metadata_rowid
};
