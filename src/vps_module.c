#include "vps_module.h"
SQLITE_EXTENSION_INIT3

#include "vps_arguments.h"
#include "vps_connection_string.h"
#include "vps_connection_pool.h"
#include "vps_identity.h"
#include "vps_libpq_client.h"
#include "vps_libpq_client_conninfo.h"
#include "vps_query_validation.h"
#include "vps_query_metadata.h"
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

typedef enum VpsCursorState {
    VPS_CURSOR_NEW = 0,
    VPS_CURSOR_OPEN = 1,
    VPS_CURSOR_FILTERING = 2,
    VPS_CURSOR_ROW_READY = 3,
    VPS_CURSOR_EOF = 4,
    VPS_CURSOR_FAILED = 5,
    VPS_CURSOR_CLOSED = 6
} VpsCursorState;

typedef struct VpsTable {
    sqlite3_vtab base;
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
    VpsModuleContext *module_context;
    uint16_t key_columns[VPS_QUERY_METADATA_MAX_KEY_COLUMNS];
    size_t key_column_count;
    VpsRowIdentityMode identity_mode;
    uint64_t table_id;
    uint64_t next_cursor_id;
    size_t active_cursors;
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
    VpsCursorState state;
    uint64_t cursor_id;
    uint64_t scan_counter;
    int64_t rowid;
    size_t row_bytes;
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

VpsModuleContext *vps_module_context_create(void)
{
    VpsModuleContext *context =
        (VpsModuleContext *)sqlite3_malloc64(sizeof(*context));
    if (context != NULL) (void)memset(context, 0, sizeof(*context));
    return context;
}

void vps_module_context_destroy(void *opaque)
{
    VpsModuleContext *context = (VpsModuleContext *)opaque;
    if (context == NULL) return;
    context->closing = 1;
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
                                           VpsError *error)
{
    size_t attempt;
    for (attempt = 0U; attempt < VPS_VTAB_DRIVE_LIMIT; ++attempt) {
        VpsClientPollResult poll;
        VpsClientStatus status =
            vps_client_statement_poll(statement, &poll, error);
        if (status != VPS_CLIENT_OK) return status;
        if (poll.outcome == VPS_CLIENT_POLL_COMPLETE ||
            poll.outcome == VPS_CLIENT_POLL_ROW_READY)
            return VPS_CLIENT_OK;
        if (poll.outcome != VPS_CLIENT_POLL_WAIT)
            return VPS_CLIENT_BACKEND_ERROR;
        status = vps_client_statement_wait(statement, &poll.wait, error);
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

static int vps_table_cleanup(VpsTable *table)
{
    int clean = 1;
    if (table == NULL) return SQLITE_OK;
    if (table->active_cursors != 0U) return SQLITE_BUSY;
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
    if (status == VPS_CLIENT_OK) status = vps_drive_statement(statement, error);
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

static int vps_module_best_index(sqlite3_vtab *table,
                                 sqlite3_index_info *index_info)
{
    (void)table;
    if (index_info == NULL) return SQLITE_MISUSE;
    index_info->estimatedCost = 1000000.0;
    index_info->estimatedRows = 1000000;
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
    cursor->state = VPS_CURSOR_OPEN;
    cursor->cursor_id = ++table->next_cursor_id;
    table->active_cursors += 1U;
    *cursor_out = &cursor->base;
    return SQLITE_OK;
}

static int vps_cursor_release(VpsCursor *cursor)
{
    int clean = 1;
    VpsConnectionLeaseDisposition disposition =
        cursor->state == VPS_CURSOR_EOF ? VPS_CONNECTION_LEASE_CLEAN
                                        : VPS_CONNECTION_LEASE_DIRTY;
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
    cursor->row = NULL;
    cursor->state = VPS_CURSOR_CLOSED;
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
    if (cursor->row != NULL) {
        status = vps_client_statement_row_consumed(cursor->statement, error);
        cursor->row = NULL;
        if (status != VPS_CLIENT_OK) goto failed;
    }
    status = vps_client_statement_start(cursor->statement,
                                        VPS_CLIENT_OPERATION_FETCH, error);
    if (status == VPS_CLIENT_OK)
        status = vps_drive_statement(cursor->statement, error);
    if (status != VPS_CLIENT_OK) goto failed;
    if (vps_client_statement_state(cursor->statement) ==
        VPS_CLIENT_STATEMENT_COMPLETE) {
        cursor->state = VPS_CURSOR_EOF;
        return SQLITE_OK;
    }
    status = vps_client_statement_current_row(cursor->statement, &row, error);
    if (status != VPS_CLIENT_OK ||
        vps_client_row_column_count(row) != cursor->table->described.field_count)
        goto failed;
    cursor->row_bytes = 0U;
    for (index = 0U; index < vps_client_row_column_count(row); ++index) {
        VpsClientColumnView column;
        if (vps_client_row_column(row, index, &column, error) != VPS_CLIENT_OK ||
            column.length > SIZE_MAX - cursor->row_bytes)
            goto failed;
        cursor->row_bytes += column.length;
    }
    if (cursor->table->identity_mode == VPS_ROW_IDENTITY_STABLE_INTEGER) {
        VpsClientColumnView key;
        if (vps_client_row_column(
                row, cursor->table->key_columns[0], &key, error) !=
                VPS_CLIENT_OK ||
            vps_row_identity_stable_integer(&key, &cursor->rowid) !=
                VPS_ROW_IDENTITY_OK)
            goto failed;
    } else if (vps_row_identity_scan_next(&cursor->scan_counter,
                                          &cursor->rowid) !=
               VPS_ROW_IDENTITY_OK) goto failed;
    cursor->row = row;
    cursor->state = VPS_CURSOR_ROW_READY;
    return SQLITE_OK;
failed:
    cursor->state = VPS_CURSOR_FAILED;
    return vps_vtab_set_error(&cursor->table->base, SQLITE_ERROR, error,
                              "VirtualPostgreSQL scan failed");
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
    (void)index_number;
    (void)index_string;
    (void)arguments;
    if (cursor == NULL || argument_count != 0 ||
        (cursor->state != VPS_CURSOR_OPEN && cursor->state != VPS_CURSOR_EOF))
        return SQLITE_MISUSE;
    table = cursor->table;
    (void)vps_cursor_release(cursor);
    cursor->state = VPS_CURSOR_FILTERING;
    cursor->scan_counter = 0U;
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
    spec.query = (const char *)table->scan_query.data;
    spec.query_length = table->scan_query.size;
    spec.result_fields = table->expected_fields;
    spec.result_field_count = table->described.field_count;
    spec.timeout_ms = 60000U;
    spec.single_row = 1;
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_open(cursor->connection, &spec,
                                           &cursor->statement, &error);
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_start(cursor->statement,
                                            VPS_CLIENT_OPERATION_EXECUTE,
                                            &error);
    if (status == VPS_CLIENT_OK)
        status = vps_drive_statement(cursor->statement, &error);
    result = status == VPS_CLIENT_OK ? vps_cursor_advance(cursor, &error)
                                    : vps_vtab_set_error(
                                          &table->base, SQLITE_ERROR, &error,
                                          "VirtualPostgreSQL filter failed");
    if (status != VPS_CLIENT_OK) cursor->state = VPS_CURSOR_FAILED;
    if (error_initialized) vps_error_reset(&error);
    return result;
}

static int vps_module_next(sqlite3_vtab_cursor *base)
{
    VpsCursor *cursor = (VpsCursor *)base;
    VpsError error;
    int result;
    if (cursor == NULL || cursor->state != VPS_CURSOR_ROW_READY)
        return SQLITE_MISUSE;
    if (vps_error_init(&error, &cursor->table->allocator) != VPS_MEMORY_OK)
        return SQLITE_NOMEM;
    result = vps_cursor_advance(cursor, &error);
    vps_error_reset(&error);
    return result;
}

static int vps_module_eof(sqlite3_vtab_cursor *base)
{
    const VpsCursor *cursor = (const VpsCursor *)base;
    return cursor == NULL || cursor->state == VPS_CURSOR_EOF;
}

static int vps_module_column(sqlite3_vtab_cursor *base,
                             sqlite3_context *context,
                             int column_index)
{
    VpsCursor *cursor = (VpsCursor *)base;
    VpsClientColumnView column;
    VpsDecodedValue decoded;
    VpsCodecId codec;
    VpsError error;
    VpsTypeCodecResult result;
    if (cursor == NULL || context == NULL || cursor->row == NULL ||
        column_index < 0 ||
        (size_t)column_index > cursor->table->described.field_count)
        return SQLITE_MISUSE;
    if ((size_t)column_index == cursor->table->described.field_count) {
        VpsClientColumnView keys[VPS_QUERY_METADATA_MAX_KEY_COLUMNS];
        VpsBuffer token;
        size_t key_index;
        if (cursor->table->identity_mode != VPS_ROW_IDENTITY_HIDDEN_TOKEN)
            return SQLITE_MISUSE;
        (void)memset(keys, 0, sizeof(keys));
        for (key_index = 0U;
             key_index < cursor->table->key_column_count; ++key_index) {
            if (vps_client_row_column(
                    cursor->row, cursor->table->key_columns[key_index],
                    &keys[key_index], NULL) != VPS_CLIENT_OK)
                return SQLITE_ERROR;
        }
        if (vps_row_identity_token(&cursor->table->allocator, keys,
                                   cursor->table->key_column_count,
                                   &token) != VPS_ROW_IDENTITY_OK)
            return SQLITE_ERROR;
        sqlite3_result_blob64(context, token.data, (sqlite3_uint64)token.size,
                              SQLITE_TRANSIENT);
        vps_buffer_reset(&token);
        return SQLITE_OK;
    }
    if (vps_error_init(&error, &cursor->table->allocator) != VPS_MEMORY_OK)
        return SQLITE_NOMEM;
    if (vps_client_row_column(cursor->row, (size_t)column_index, &column,
                              &error) != VPS_CLIENT_OK) {
        vps_error_reset(&error);
        return SQLITE_ERROR;
    }
    codec = vps_type_codec_for_oid(column.type_oid);
    result = vps_type_codec_decode(&cursor->table->allocator, codec, &column,
                                   &decoded);
    if (result != VPS_TYPE_CODEC_OK) {
        vps_error_reset(&error);
        return vps_vtab_set_error(&cursor->table->base, SQLITE_MISMATCH, NULL,
                                  "VirtualPostgreSQL column conversion failed");
    }
    switch (decoded.kind) {
        case VPS_DECODED_NULL: sqlite3_result_null(context); break;
        case VPS_DECODED_INTEGER:
            sqlite3_result_int64(context, (sqlite3_int64)decoded.integer);
            break;
        case VPS_DECODED_REAL: sqlite3_result_double(context, decoded.real); break;
        case VPS_DECODED_TEXT:
            sqlite3_result_text64(context, (const char *)decoded.bytes,
                                  (sqlite3_uint64)decoded.length,
                                  SQLITE_TRANSIENT, SQLITE_UTF8);
            break;
        case VPS_DECODED_BLOB:
            sqlite3_result_blob64(context, decoded.bytes,
                                  (sqlite3_uint64)decoded.length,
                                  SQLITE_TRANSIENT);
            break;
        default: sqlite3_result_error(context, "invalid codec state", -1); break;
    }
    vps_decoded_value_reset(&decoded);
    vps_error_reset(&error);
    return SQLITE_OK;
}

static int vps_module_rowid(sqlite3_vtab_cursor *base,
                            sqlite3_int64 *rowid_out)
{
    const VpsCursor *cursor = (const VpsCursor *)base;
    if (cursor == NULL || rowid_out == NULL ||
        cursor->state != VPS_CURSOR_ROW_READY) return SQLITE_MISUSE;
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
    .xBestIndex = vps_module_best_index,
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
