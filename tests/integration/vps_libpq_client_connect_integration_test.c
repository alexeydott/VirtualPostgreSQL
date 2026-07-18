#include "vps_libpq_client.h"
#include "vps_libpq_client_metadata.h"
#include "vps_connection_pool.h"
#include "vps_schema_fingerprint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VPS_ASYNC_ENV_HOST "VPS_ASYNC_TEST_HOST"
#define VPS_ASYNC_ENV_PORT "VPS_ASYNC_TEST_PORT"
#define VPS_ASYNC_ENV_USER "VPS_ASYNC_TEST_USER"
#define VPS_ASYNC_ENV_PASSWORD "VPS_ASYNC_TEST_PASSWORD"
#define VPS_ASYNC_ENV_DBNAME "VPS_ASYNC_TEST_DBNAME"
#define VPS_ASYNC_ENV_SSLMODE "VPS_ASYNC_TEST_SSLMODE"
#define VPS_ASYNC_ENV_SSLROOTCERT "VPS_ASYNC_TEST_SSLROOTCERT"
#define VPS_ASYNC_ENV_CHANNEL_BINDING "VPS_ASYNC_TEST_CHANNEL_BINDING"
#define VPS_ASYNC_ENV_FIXTURE "VPS_ASYNC_TEST_FIXTURE"
#define VPS_ASYNC_ENV_NETWORK_LOSS "VPS_ASYNC_TEST_NETWORK_LOSS"
#define VPS_ASYNC_ENV_METADATA_SCHEMA "VPS_ASYNC_TEST_METADATA_SCHEMA"
#define VPS_ASYNC_ENV_METADATA_TABLE "VPS_ASYNC_TEST_METADATA_TABLE"

typedef struct VpsAsyncEnvironment {
    const char *host;
    const char *port;
    const char *user;
    const char *password;
    const char *dbname;
    const char *sslmode;
    const char *sslrootcert;
    const char *channel_binding;
} VpsAsyncEnvironment;

typedef struct VpsAsyncLogCapture {
    const VpsAsyncEnvironment *environment;
    size_t event_count;
    int sensitive_value_seen;
    char last_phase[64];
    char last_status[64];
    char failure_phase[64];
    char failure_status[64];
} VpsAsyncLogCapture;

typedef struct VpsAsyncApiContext {
    const VpsLibpqClientApi *production;
    size_t finish_count;
} VpsAsyncApiContext;

static int vps_async_environment(VpsAsyncEnvironment *environment)
{
    environment->host = getenv(VPS_ASYNC_ENV_HOST);
    environment->port = getenv(VPS_ASYNC_ENV_PORT);
    environment->user = getenv(VPS_ASYNC_ENV_USER);
    environment->password = getenv(VPS_ASYNC_ENV_PASSWORD);
    environment->dbname = getenv(VPS_ASYNC_ENV_DBNAME);
    environment->sslmode = getenv(VPS_ASYNC_ENV_SSLMODE);
    environment->sslrootcert = getenv(VPS_ASYNC_ENV_SSLROOTCERT);
    environment->channel_binding = getenv(VPS_ASYNC_ENV_CHANNEL_BINDING);
    return environment->host != NULL && environment->port != NULL &&
           environment->user != NULL && environment->password != NULL &&
           environment->dbname != NULL && environment->sslmode != NULL &&
           environment->channel_binding != NULL;
}

static int vps_async_string_equals(const VpsLogString *logged,
                                   const char *sensitive)
{
    size_t length;
    if (logged == NULL || sensitive == NULL) return 0;
    length = strlen(sensitive);
    return length != 0U && logged->length == length &&
           memcmp(logged->data, sensitive, length) == 0;
}

static int vps_async_log_sink(void *context, const VpsLogEvent *event)
{
    VpsAsyncLogCapture *capture = (VpsAsyncLogCapture *)context;
    char operation[64] = {0};
    char phase[64] = {0};
    char status[64] = {0};
    size_t index;
    capture->event_count += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->type != VPS_LOG_FIELD_TYPE_STRING) continue;
        if ((field->key == VPS_LOG_FIELD_OPERATION ||
             field->key == VPS_LOG_FIELD_PHASE ||
             field->key == VPS_LOG_FIELD_STATUS) &&
            field->value.string_value.length < sizeof(operation)) {
            char *destination = field->key == VPS_LOG_FIELD_OPERATION
                                    ? operation
                                    : field->key == VPS_LOG_FIELD_PHASE
                                          ? phase
                                          : status;
            (void)memcpy(destination, field->value.string_value.data,
                         field->value.string_value.length);
            destination[field->value.string_value.length] = '\0';
        }
        if (vps_async_string_equals(&field->value.string_value,
                                    capture->environment->host) ||
            vps_async_string_equals(&field->value.string_value,
                                    capture->environment->user) ||
            vps_async_string_equals(&field->value.string_value,
                                    capture->environment->password) ||
            vps_async_string_equals(&field->value.string_value,
                                    capture->environment->dbname) ||
            vps_async_string_equals(&field->value.string_value,
                                    capture->environment->sslrootcert)) {
            capture->sensitive_value_seen = 1;
        }
    }
    if (phase[0] != '\0') (void)strcpy(capture->last_phase, phase);
    if (status[0] != '\0') (void)strcpy(capture->last_status, status);
    if (event->level >= VPS_LOG_LEVEL_WARN &&
        capture->failure_phase[0] == '\0') {
        (void)strcpy(capture->failure_phase, phase);
        (void)strcpy(capture->failure_status, status);
    }
    return 0;
}

static void vps_async_finish(void *context, void *connection)
{
    VpsAsyncApiContext *api_context = (VpsAsyncApiContext *)context;
    api_context->production->finish(NULL, connection);
    api_context->finish_count += 1U;
}

static int vps_async_arguments(VpsParsedArguments *arguments,
                               int fixture_requested)
{
    const char *values[] = {
        "service=async_probe", "source=table", "schema=public",
        "table=async_probe", fixture_requested ? "mode=rw" : "mode=ro"};
    VpsArgumentInput inputs[sizeof(values) / sizeof(values[0])];
    size_t index;
    for (index = 0U; index < sizeof(values) / sizeof(values[0]); ++index) {
        inputs[index].text = values[index];
        inputs[index].length = strlen(values[index]);
    }
    return vps_arguments_parse(arguments, inputs,
                               sizeof(inputs) / sizeof(inputs[0]), NULL) ==
           VPS_ARGUMENTS_OK;
}

static void vps_async_configure(VpsConnectionConfig *config,
                                const VpsAsyncEnvironment *environment)
{
    config->mode = VPS_CONNECTION_MODE_PROFILE;
    config->config.header.present_fields =
        VPS_CREDENTIAL_FIELD_HOSTS | VPS_CREDENTIAL_FIELD_PORTS |
        VPS_CREDENTIAL_FIELD_USER | VPS_CREDENTIAL_FIELD_PASSWORD |
        VPS_CREDENTIAL_FIELD_DBNAME | VPS_CREDENTIAL_FIELD_SSLMODE |
        VPS_CREDENTIAL_FIELD_CHANNEL_BINDING |
        VPS_CREDENTIAL_FIELD_CONNECT_TIMEOUT |
        VPS_CREDENTIAL_FIELD_STATEMENT_TIMEOUT |
        VPS_CREDENTIAL_FIELD_LOCK_TIMEOUT |
        VPS_CREDENTIAL_FIELD_APPLICATION_NAME |
        VPS_CREDENTIAL_FIELD_SEARCH_PATH;
    config->config.hosts = environment->host;
    config->config.ports = environment->port;
    config->config.user = environment->user;
    config->config.password = environment->password;
    config->config.dbname = environment->dbname;
    config->config.sslmode = environment->sslmode;
    config->config.channel_binding = environment->channel_binding;
    config->config.connect_timeout = "10";
    config->config.statement_timeout = "1500";
    config->config.lock_timeout = "250";
    config->config.application_name = "VirtualPostgreSQL/async-probe";
    config->config.search_path = "pg_catalog";
    if (environment->sslrootcert != NULL &&
        environment->sslrootcert[0] != '\0') {
        config->config.header.present_fields |=
            VPS_CREDENTIAL_FIELD_SSLROOTCERT;
        config->config.sslrootcert = environment->sslrootcert;
    }
}

static VpsClientStatus vps_async_drive(VpsClientConnection *connection,
                                       VpsError *error)
{
    VpsClientPollResult poll_result;
    size_t attempt;
    for (attempt = 0U; attempt < 1024U; ++attempt) {
        VpsClientStatus status = vps_client_connection_poll(
            connection, &poll_result, error);
        if (status != VPS_CLIENT_OK) return status;
        if (poll_result.outcome == VPS_CLIENT_POLL_COMPLETE) {
            return VPS_CLIENT_OK;
        }
        if (poll_result.outcome != VPS_CLIENT_POLL_WAIT) {
            return VPS_CLIENT_BACKEND_ERROR;
        }
        status = vps_client_connection_wait(connection, &poll_result.wait,
                                            error);
        if (status != VPS_CLIENT_OK) return status;
    }
    return VPS_CLIENT_LIMIT_EXCEEDED;
}

static VpsClientStatus vps_async_drive_statement(
    VpsClientStatement *statement,
    VpsError *error)
{
    VpsClientPollResult poll_result;
    size_t attempts;
    for (attempts = 0U; attempts < 512U; ++attempts) {
        VpsClientStatus status = vps_client_statement_poll(
            statement, &poll_result, error);
        if (status != VPS_CLIENT_OK) return status;
        if (poll_result.outcome == VPS_CLIENT_POLL_COMPLETE ||
            poll_result.outcome == VPS_CLIENT_POLL_ROW_READY) {
            return VPS_CLIENT_OK;
        }
        if (poll_result.outcome != VPS_CLIENT_POLL_WAIT) {
            return VPS_CLIENT_BACKEND_ERROR;
        }
        status = vps_client_statement_wait(statement, &poll_result.wait,
                                           error);
        if (status != VPS_CLIENT_OK) return status;
    }
    return VPS_CLIENT_LIMIT_EXCEEDED;
}

static VpsClientStatus vps_async_execute_command(
    VpsClientConnection *connection,
    const char *query,
    VpsError *error)
{
    VpsClientStatementSpec spec;
    VpsClientStatement *statement = NULL;
    VpsClientStatus status;
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = query;
    spec.query_length = strlen(query);
    spec.timeout_ms = 5000U;
    status = vps_client_statement_open(connection, &spec, &statement, error);
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_EXECUTE, error);
    }
    if (status == VPS_CLIENT_OK) {
        status = vps_async_drive_statement(statement, error);
    }
    if (vps_client_statement_close(&statement) != VPS_CLIENT_OK &&
        status == VPS_CLIENT_OK) {
        status = VPS_CLIENT_BACKEND_ERROR;
    }
    return status;
}

typedef struct VpsAsyncMetadataRow {
    VpsClientColumnView columns[VPS_METADATA_MAX_FIELDS];
} VpsAsyncMetadataRow;

static int vps_async_metadata_is_null(void *context, size_t row, size_t field)
{
    VpsAsyncMetadataRow *input = (VpsAsyncMetadataRow *)context;
    (void)row;
    return input->columns[field].is_null;
}

static const void *vps_async_metadata_value(void *context,
                                            size_t row,
                                            size_t field)
{
    VpsAsyncMetadataRow *input = (VpsAsyncMetadataRow *)context;
    (void)row;
    return input->columns[field].data;
}

static size_t vps_async_metadata_length(void *context,
                                        size_t row,
                                        size_t field)
{
    VpsAsyncMetadataRow *input = (VpsAsyncMetadataRow *)context;
    (void)row;
    return input->columns[field].length;
}

static VpsClientStatus vps_async_catalog_fetch(
    VpsClientConnection *connection,
    VpsCatalogQuery query,
    const VpsClientParameterView *parameters,
    size_t parameter_count,
    VpsMetadataRowSet *rowset,
    VpsError *error)
{
    VpsLibpqMetadataStatement metadata_statement;
    VpsClientStatement *statement = NULL;
    VpsClientStatus status;
    VpsMetadataInput input;
    VpsAsyncMetadataRow input_row;
    size_t rows = 0U;
    (void)memset(&input, 0, sizeof(input));
    (void)memset(&input_row, 0, sizeof(input_row));
    if (vps_libpq_metadata_statement_init(
            &metadata_statement, query, parameters, parameter_count, 5000U) !=
        VPS_METADATA_OK) return VPS_CLIENT_INVALID_ARGUMENT;
    input.context = &input_row;
    input.field_count = metadata_statement.statement.result_field_count;
    input.is_null = vps_async_metadata_is_null;
    input.value = vps_async_metadata_value;
    input.length = vps_async_metadata_length;
    status = vps_client_statement_open(connection,
                                       &metadata_statement.statement,
                                       &statement, error);
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_start(statement,
                                            VPS_CLIENT_OPERATION_EXECUTE,
                                            error);
    if (status == VPS_CLIENT_OK) status = vps_async_drive_statement(statement, error);
    while (status == VPS_CLIENT_OK &&
           vps_client_statement_state(statement) != VPS_CLIENT_STATEMENT_COMPLETE) {
        const VpsClientRowView *row = NULL;
        size_t field;
        status = vps_client_statement_start(statement,
                                            VPS_CLIENT_OPERATION_FETCH,
                                            error);
        if (status == VPS_CLIENT_OK)
            status = vps_async_drive_statement(statement, error);
        if (status != VPS_CLIENT_OK ||
            vps_client_statement_state(statement) == VPS_CLIENT_STATEMENT_COMPLETE)
            break;
        status = vps_client_statement_current_row(statement, &row, error);
        if (status == VPS_CLIENT_OK &&
            vps_client_row_column_count(row) != input.field_count)
            status = VPS_CLIENT_BACKEND_ERROR;
        for (field = 0U; status == VPS_CLIENT_OK && field < input.field_count;
             ++field)
            status = vps_client_row_column(row, field,
                                           &input_row.columns[field], error);
        input.row_count = 1U;
        if (status == VPS_CLIENT_OK &&
            vps_metadata_rowset_append(rowset, query, &input) != VPS_METADATA_OK)
            status = VPS_CLIENT_BACKEND_ERROR;
        if (status == VPS_CLIENT_OK) {
            ++rows;
            status = vps_client_statement_row_consumed(statement, error);
        }
    }
    if (status == VPS_CLIENT_OK && rows == 0U) {
        input.row_count = 0U;
        if (vps_metadata_rowset_copy(rowset, query, &input) != VPS_METADATA_OK)
            status = VPS_CLIENT_BACKEND_ERROR;
    }
    if (vps_client_statement_close(&statement) != VPS_CLIENT_OK &&
        status == VPS_CLIENT_OK) status = VPS_CLIENT_BACKEND_ERROR;
    return status;
}

static VpsClientStatus vps_async_metadata_probe(
    VpsClientConnection *connection,
    const VpsAllocator *allocator,
    VpsLogger *logger,
    VpsError *error)
{
    static const char default_schema[] = "pg_catalog";
    static const char default_relation[] = "pg_class";
    const char *schema = getenv(VPS_ASYNC_ENV_METADATA_SCHEMA);
    const char *relation_name = getenv(VPS_ASYNC_ENV_METADATA_TABLE);
    VpsClientParameterView name_parameters[2];
    VpsClientParameterView oid_parameter;
    VpsMetadataRowSet relation_rows;
    VpsMetadataRowSet column_rows;
    VpsMetadataRowSet key_rows;
    VpsMetadataRowSet policy_rows;
    VpsRelationMetadata relation;
    VpsColumnSet columns;
    VpsKeyMetadata key;
    VpsRelationPolicyMetadata policy;
    VpsTypeRegistry registry;
    VpsSchemaFingerprintInput fingerprint_input;
    VpsSchemaFingerprint fingerprint;
    VpsClientStatus status = VPS_CLIENT_BACKEND_ERROR;
    char oid[16];
    int oid_length;
    const char *metadata_phase = "init";
    int relation_initialized = 0;
    int columns_initialized = 0;
    if (schema == NULL || schema[0] == '\0') schema = default_schema;
    if (relation_name == NULL || relation_name[0] == '\0')
        relation_name = default_relation;
    (void)memset(&relation_rows, 0, sizeof(relation_rows));
    (void)memset(&column_rows, 0, sizeof(column_rows));
    (void)memset(&key_rows, 0, sizeof(key_rows));
    (void)memset(&policy_rows, 0, sizeof(policy_rows));
    (void)memset(&relation, 0, sizeof(relation));
    (void)memset(&columns, 0, sizeof(columns));
    (void)memset(&key, 0, sizeof(key));
    (void)memset(&policy, 0, sizeof(policy));
    (void)memset(&fingerprint_input, 0, sizeof(fingerprint_input));
    (void)memset(name_parameters, 0, sizeof(name_parameters));
    (void)memset(&oid_parameter, 0, sizeof(oid_parameter));
    if (strlen(schema) > VPS_METADATA_NAME_MAX_BYTES ||
        strlen(relation_name) > VPS_METADATA_NAME_MAX_BYTES ||
        vps_metadata_rowset_init(&relation_rows, allocator, logger) !=
            VPS_METADATA_OK ||
        vps_metadata_rowset_init(&column_rows, allocator, logger) !=
            VPS_METADATA_OK ||
        vps_metadata_rowset_init(&key_rows, allocator, logger) !=
            VPS_METADATA_OK ||
        vps_metadata_rowset_init(&policy_rows, allocator, logger) !=
            VPS_METADATA_OK)
        goto cleanup;
    name_parameters[0].value = schema;
    name_parameters[0].length = strlen(schema);
    name_parameters[0].type_oid = VPS_METADATA_NAME_OID;
    name_parameters[0].format = VPS_CLIENT_VALUE_TEXT;
    name_parameters[1].value = relation_name;
    name_parameters[1].length = strlen(relation_name);
    name_parameters[1].type_oid = VPS_METADATA_NAME_OID;
    name_parameters[1].format = VPS_CLIENT_VALUE_TEXT;
    metadata_phase = "relation_query";
    status = vps_async_catalog_fetch(connection, VPS_CATALOG_QUERY_RELATION,
                                     name_parameters, 2U, &relation_rows,
                                     error);
    if (status != VPS_CLIENT_OK ||
        vps_relation_metadata_init(&relation, allocator, logger) !=
            VPS_METADATA_OK)
        goto cleanup;
    relation_initialized = 1;
    metadata_phase = "relation_resolve";
    if (vps_relation_metadata_resolve(&relation, &relation_rows, schema,
                                      strlen(schema), relation_name,
                                      strlen(relation_name)) != VPS_METADATA_OK) {
        status = VPS_CLIENT_BACKEND_ERROR;
        goto cleanup;
    }
    oid_length = snprintf(oid, sizeof(oid), "%u",
                          (unsigned int)relation.relation_oid);
    if (oid_length <= 0 || (size_t)oid_length >= sizeof(oid)) {
        status = VPS_CLIENT_BACKEND_ERROR;
        goto cleanup;
    }
    oid_parameter.value = oid;
    oid_parameter.length = (size_t)oid_length;
    oid_parameter.type_oid = 26U;
    oid_parameter.format = VPS_CLIENT_VALUE_TEXT;
    metadata_phase = "columns_query";
    status = vps_async_catalog_fetch(connection, VPS_CATALOG_QUERY_COLUMNS,
                                     &oid_parameter, 1U, &column_rows, error);
    if (status == VPS_CLIENT_OK) {
        metadata_phase = "keys_query";
        status = vps_async_catalog_fetch(connection, VPS_CATALOG_QUERY_KEYS,
                                         &oid_parameter, 1U, &key_rows, error);
    }
    if (status == VPS_CLIENT_OK) {
        metadata_phase = "policy_query";
        status = vps_async_catalog_fetch(
            connection, VPS_CATALOG_QUERY_RELATION_POLICY, &oid_parameter,
            1U, &policy_rows, error);
    }
    if (status != VPS_CLIENT_OK ||
        vps_column_set_init(&columns, allocator, logger) != VPS_METADATA_OK)
        goto cleanup;
    columns_initialized = 1;
    metadata_phase = "columns_build";
    if (vps_column_set_build(&columns, &column_rows) != VPS_METADATA_OK) {
        status = VPS_CLIENT_BACKEND_ERROR;
        goto cleanup;
    }
    metadata_phase = "key_discovery";
    if (vps_key_discover(&key_rows, &columns, NULL, 0U, 0, logger, &key) !=
        VPS_METADATA_OK) {
        status = VPS_CLIENT_BACKEND_ERROR;
        goto cleanup;
    }
    metadata_phase = "policy_build";
    if (vps_relation_policy_build(&relation, &key, &policy_rows, logger,
                                  &policy) != VPS_METADATA_OK) {
        status = VPS_CLIENT_BACKEND_ERROR;
        goto cleanup;
    }
    metadata_phase = "registry_init";
    if (vps_type_registry_init(&registry, logger) != VPS_METADATA_OK) {
        status = VPS_CLIENT_BACKEND_ERROR;
        goto cleanup;
    }
    fingerprint_input.relation = &relation;
    fingerprint_input.columns = &columns;
    fingerprint_input.key = &key;
    fingerprint_input.policy = &policy;
    fingerprint_input.type_registry = &registry;
    fingerprint_input.spatial.metadata_version = 1U;
    metadata_phase = "fingerprint";
    if (vps_schema_fingerprint_build(&fingerprint_input, logger,
                                     &fingerprint) != VPS_METADATA_OK) {
        status = VPS_CLIENT_BACKEND_ERROR;
        goto cleanup;
    }
    (void)printf(
        "async_metadata_probe status=ok relation_oid=%u columns=%u key=%s policy=%s fingerprint_version=%u fingerprint=%s\n",
        (unsigned int)relation.relation_oid,
        (unsigned int)columns.visible_count, vps_key_source_name(key.source),
        vps_relation_write_policy_name(policy.write_policy),
        (unsigned int)fingerprint.version, fingerprint.hex);
    status = VPS_CLIENT_OK;
cleanup:
    if (status != VPS_CLIENT_OK)
        (void)printf(
            "async_metadata_probe status=failed phase=%s relation_rows=%u relation_fields=%u column_rows=%u key_rows=%u policy_rows=%u\n",
            metadata_phase, (unsigned int)relation_rows.row_count,
            (unsigned int)relation_rows.field_count,
            (unsigned int)column_rows.row_count,
            (unsigned int)key_rows.row_count,
            (unsigned int)policy_rows.row_count);
    if (columns_initialized) vps_column_set_reset(&columns);
    if (relation_initialized) vps_relation_metadata_reset(&relation);
    vps_metadata_rowset_reset(&policy_rows);
    vps_metadata_rowset_reset(&key_rows);
    vps_metadata_rowset_reset(&column_rows);
    vps_metadata_rowset_reset(&relation_rows);
    return status;
}

static VpsClientStatus vps_async_fixture_bootstrap_and_verify(
    VpsClientConnection *connection,
    VpsError *error)
{
    static const char bootstrap[] =
        "DO $vps$ BEGIN "
        "CREATE SCHEMA IF NOT EXISTS vps_stage4_control; "
        "CREATE TABLE IF NOT EXISTS vps_stage4_control.fixture_rows ("
        "id pg_catalog.int8 PRIMARY KEY, control_code pg_catalog.text NOT NULL, "
        "signed_value pg_catalog.int4 NOT NULL, exact_value pg_catalog.numeric(12,3) NOT NULL, "
        "active pg_catalog.bool NOT NULL, control_date pg_catalog.date NOT NULL, "
        "observed_at pg_catalog.timestamptz NOT NULL, payload pg_catalog.jsonb NOT NULL, "
        "binary_value pg_catalog.bytea NOT NULL, control_uuid pg_catalog.uuid NOT NULL); "
        "TRUNCATE TABLE vps_stage4_control.fixture_rows; "
        "INSERT INTO vps_stage4_control.fixture_rows VALUES "
        "(1,'alpha',-5,12.345,true,DATE '2024-02-29',TIMESTAMPTZ '2024-02-29 12:34:56+00',"
        "'{\"kind\":\"alpha\",\"rank\":1}'::pg_catalog.jsonb,decode('00017fff','hex'),"
        "'00000000-0000-4000-8000-000000000001'::pg_catalog.uuid),"
        "(2,'beta',10,0.000,false,DATE '2025-12-31',TIMESTAMPTZ '2025-12-31 23:59:59+00',"
        "'{\"kind\":\"beta\",\"rank\":2}'::pg_catalog.jsonb,decode('deadbeef','hex'),"
        "'00000000-0000-4000-8000-000000000002'::pg_catalog.uuid),"
        "(3,'gamma',30,999999.999,true,DATE '2026-07-18',TIMESTAMPTZ '2026-07-18 00:00:00+00',"
        "'{\"kind\":\"gamma\",\"rank\":3}'::pg_catalog.jsonb,decode('ff00aa55','hex'),"
        "'00000000-0000-4000-8000-000000000003'::pg_catalog.uuid); "
        "CREATE INDEX IF NOT EXISTS fixture_rows_code_idx ON vps_stage4_control.fixture_rows (control_code); "
        "CREATE INDEX IF NOT EXISTS fixture_rows_active_idx ON vps_stage4_control.fixture_rows (id) WHERE active; "
        "CREATE INDEX IF NOT EXISTS fixture_rows_payload_gin_idx ON vps_stage4_control.fixture_rows USING gin (payload); "
        "END $vps$";
    static const char verify[] =
        "SELECT pg_catalog.concat(pg_catalog.count(*)::pg_catalog.text,'|',"
        "pg_catalog.sum(signed_value)::pg_catalog.text,'|',"
        "pg_catalog.min(control_code),'|',pg_catalog.max(control_date)::pg_catalog.text,'|',"
        "(SELECT pg_catalog.count(*) FROM pg_catalog.pg_indexes WHERE schemaname='vps_stage4_control' "
        "AND indexname IN ('fixture_rows_code_idx','fixture_rows_active_idx','fixture_rows_payload_gin_idx'))::pg_catalog.text) "
        "FROM vps_stage4_control.fixture_rows";
    static const char expected[] = "3|35|alpha|2026-07-18|3";
    VpsClientStatementSpec spec;
    VpsClientResultFieldExpectation field;
    VpsClientStatement *statement = NULL;
    const VpsClientRowView *row = NULL;
    VpsClientColumnView column;
    VpsClientStatus status = vps_async_execute_command(
        connection, bootstrap, error);
    if (status != VPS_CLIENT_OK) return status;
    (void)memset(&spec, 0, sizeof(spec));
    (void)memset(&column, 0, sizeof(column));
    field.type_oid = 25U;
    field.format = VPS_CLIENT_VALUE_TEXT;
    spec.query = verify;
    spec.query_length = sizeof(verify) - 1U;
    spec.result_fields = &field;
    spec.result_field_count = 1U;
    spec.timeout_ms = 5000U;
    spec.single_row = 1;
    status = vps_client_statement_open(connection, &spec, &statement, error);
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_EXECUTE, error);
    }
    if (status == VPS_CLIENT_OK) status = vps_async_drive_statement(statement, error);
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_FETCH, error);
    }
    if (status == VPS_CLIENT_OK) status = vps_async_drive_statement(statement, error);
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_current_row(statement, &row, error);
    }
    if (status == VPS_CLIENT_OK) {
        status = vps_client_row_column(row, 0U, &column, error);
    }
    if (status == VPS_CLIENT_OK && !column.is_null && column.data != NULL &&
        column.length <= 64U) {
        (void)printf("async_fixture_digest value=%.*s expected=%s\n",
                     (int)column.length, (const char *)column.data, expected);
    }
    if (status == VPS_CLIENT_OK &&
        (column.is_null || column.length != sizeof(expected) - 1U ||
         memcmp(column.data, expected, sizeof(expected) - 1U) != 0)) {
        status = VPS_CLIENT_BACKEND_ERROR;
    }
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_row_consumed(statement, error);
    }
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_FETCH, error);
    }
    if (status == VPS_CLIENT_OK) status = vps_async_drive_statement(statement, error);
    if (status == VPS_CLIENT_OK &&
        vps_client_statement_state(statement) != VPS_CLIENT_STATEMENT_COMPLETE) {
        (void)printf("async_fixture_terminal state=%s\n",
                     vps_client_statement_state_name(
                         vps_client_statement_state(statement)));
        status = VPS_CLIENT_BACKEND_ERROR;
    }
    if (vps_client_statement_close(&statement) != VPS_CLIENT_OK &&
        status == VPS_CLIENT_OK) {
        status = VPS_CLIENT_BACKEND_ERROR;
    }
    return status;
}

static int vps_async_network_loss_before_row(VpsClientConnection *connection,
                                             VpsError *error)
{
    static const char query[] =
        "DO $vps$ BEGIN "
        "PERFORM pg_catalog.pg_terminate_backend(pg_catalog.pg_backend_pid()); "
        "PERFORM pg_catalog.pg_sleep(10); END $vps$";
    VpsClientStatementSpec spec;
    VpsClientStatement *statement = NULL;
    VpsClientStatus status;
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = query;
    spec.query_length = sizeof(query) - 1U;
    spec.timeout_ms = 15000U;
    status = vps_client_statement_open(connection, &spec, &statement, error);
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_EXECUTE, error);
    }
    if (status == VPS_CLIENT_OK) status = vps_async_drive_statement(statement, error);
    (void)vps_client_statement_close(&statement);
    (void)printf("async_network_before status=%s\n",
                 vps_client_status_name(status));
    return status == VPS_CLIENT_BACKEND_ERROR;
}

static int vps_async_network_loss_after_row(VpsClientConnection *connection,
                                            VpsError *error)
{
    static const char query[] =
        "SELECT CASE WHEN i=1 THEN pg_catalog.repeat('x',10000) "
        "ELSE pg_catalog.pg_terminate_backend(pg_catalog.pg_backend_pid())::pg_catalog.text END "
        "FROM pg_catalog.generate_series(1,2) AS s(i)";
    VpsClientResultFieldExpectation field = {
        25U, VPS_CLIENT_VALUE_TEXT};
    VpsClientStatementSpec spec;
    VpsClientStatement *statement = NULL;
    const VpsClientRowView *row = NULL;
    VpsClientColumnView column;
    VpsClientStatus status;
    int first_row_seen = 0;
    (void)memset(&spec, 0, sizeof(spec));
    (void)memset(&column, 0, sizeof(column));
    spec.query = query;
    spec.query_length = sizeof(query) - 1U;
    spec.result_fields = &field;
    spec.result_field_count = 1U;
    spec.timeout_ms = 5000U;
    spec.single_row = 1;
    status = vps_client_statement_open(connection, &spec, &statement, error);
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_EXECUTE, error);
    }
    if (status == VPS_CLIENT_OK) status = vps_async_drive_statement(statement, error);
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_FETCH, error);
    }
    if (status == VPS_CLIENT_OK) status = vps_async_drive_statement(statement, error);
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_current_row(statement, &row, error);
    }
    if (status == VPS_CLIENT_OK) {
        status = vps_client_row_column(row, 0U, &column, error);
    }
    if (status == VPS_CLIENT_OK && !column.is_null &&
        column.length == 10000U) {
        first_row_seen = 1;
        status = vps_client_statement_row_consumed(statement, error);
    } else if (status == VPS_CLIENT_OK) {
        status = VPS_CLIENT_BACKEND_ERROR;
    }
    if (status == VPS_CLIENT_OK) {
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_FETCH, error);
    }
    if (status == VPS_CLIENT_OK) status = vps_async_drive_statement(statement, error);
    if (status == VPS_CLIENT_OK &&
        vps_client_statement_state(statement) ==
            VPS_CLIENT_STATEMENT_ROW_READY) {
        status = vps_client_statement_row_consumed(statement, error);
        if (status == VPS_CLIENT_OK) {
            status = vps_client_statement_start(
                statement, VPS_CLIENT_OPERATION_FETCH, error);
        }
        if (status == VPS_CLIENT_OK) {
            status = vps_async_drive_statement(statement, error);
        }
    }
    (void)vps_client_statement_close(&statement);
    (void)printf("async_network_after first_row=%d bytes=%u status=%s\n",
                 first_row_seen, (unsigned int)column.length,
                 vps_client_status_name(status));
    return first_row_seen && status == VPS_CLIENT_BACKEND_ERROR;
}

static int vps_cancel_probe_settle(uint32_t timeout_ms)
{
    const VpsPlatformOperations *platform = vps_platform_current_operations();
    VpsPlatformMutex mutex = {0};
    VpsPlatformCondition condition = {0};
    VpsPlatformStatus wait_status;
    if (vps_platform_mutex_init(platform, &mutex) != VPS_PLATFORM_OK ||
        vps_platform_condition_init(platform, &condition) != VPS_PLATFORM_OK ||
        vps_platform_mutex_lock(platform, &mutex) != VPS_PLATFORM_OK) {
        (void)vps_platform_condition_destroy(platform, &condition);
        (void)vps_platform_mutex_destroy(platform, &mutex);
        return 0;
    }
    wait_status = vps_platform_condition_wait(platform, &condition, &mutex,
                                              timeout_ms);
    (void)vps_platform_mutex_unlock(platform, &mutex);
    (void)vps_platform_condition_destroy(platform, &condition);
    (void)vps_platform_mutex_destroy(platform, &mutex);
    return wait_status == VPS_PLATFORM_TIMEOUT;
}

typedef struct VpsIntegrationPoolContext {
    VpsClient *client;
    VpsError *error;
} VpsIntegrationPoolContext;

static VpsConnectionPoolResult vps_integration_pool_create(
    void *context, void **connection_out)
{
    VpsIntegrationPoolContext *pool_context =
        (VpsIntegrationPoolContext *)context;
    VpsClientConnection *connection = NULL;
    VpsClientStatus status = vps_client_connection_open(
        pool_context->client, &connection, pool_context->error);
    if (status == VPS_CLIENT_OK) {
        status = vps_client_connection_start(
            connection, VPS_CLIENT_OPERATION_CONNECT, pool_context->error);
    }
    if (status == VPS_CLIENT_OK) {
        status = vps_async_drive(connection, pool_context->error);
    }
    if (status != VPS_CLIENT_OK) {
        if (connection != NULL) {
            (void)vps_client_connection_close(&connection);
        }
        return VPS_CONNECTION_POOL_CREATE_FAILED;
    }
    *connection_out = connection;
    return VPS_CONNECTION_POOL_OK;
}

static VpsConnectionPoolResult vps_integration_pool_operation(
    void *context, void *connection_value, VpsClientOperation operation)
{
    VpsIntegrationPoolContext *pool_context =
        (VpsIntegrationPoolContext *)context;
    VpsClientConnection *connection =
        (VpsClientConnection *)connection_value;
    VpsClientStatus status = vps_client_connection_start(
        connection, operation, pool_context->error);
    if (status == VPS_CLIENT_OK) {
        status = vps_async_drive(connection, pool_context->error);
    }
    return status == VPS_CLIENT_OK ? VPS_CONNECTION_POOL_OK
                                   : VPS_CONNECTION_POOL_VALIDATE_FAILED;
}

static VpsConnectionPoolResult vps_integration_pool_validate(
    void *context, void *connection)
{
    return vps_integration_pool_operation(context, connection,
                                          VPS_CLIENT_OPERATION_PING);
}

static VpsConnectionPoolResult vps_integration_pool_reset(
    void *context, void *connection)
{
    VpsConnectionPoolResult result = vps_integration_pool_operation(
        context, connection, VPS_CLIENT_OPERATION_RESET);
    return result == VPS_CONNECTION_POOL_OK ? result
                                             : VPS_CONNECTION_POOL_RESET_FAILED;
}

static void vps_integration_pool_destroy(void *context, void *connection_value)
{
    VpsClientConnection *connection =
        (VpsClientConnection *)connection_value;
    (void)context;
    (void)vps_client_connection_close(&connection);
}

int main(void)
{
    VpsAsyncEnvironment environment;
    VpsAsyncLogCapture log_capture;
    VpsAsyncApiContext api_context;
    VpsAllocator allocator;
    VpsLogger logger;
    VpsParsedArguments arguments;
    VpsConnectionConfig config;
    VpsConnectionIdentity identity;
    VpsIdentityBuildOptions identity_options = {
        "postgresql", 10U, 1U, 1U};
    VpsTlsPolicy tls_policy;
    VpsTlsPolicyOptions tls_options;
    VpsSessionPlan session_plan;
    VpsSessionBuildOptions session_options = {NULL, 0U, 2000U};
    VpsLibpqClientApi api;
    VpsLibpqClientOptions adapter_options;
    VpsLibpqClient adapter;
    VpsClientOperations client_operations;
    VpsClient client;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    VpsClientParameterView parameter;
    VpsClientResultFieldExpectation result_field;
    VpsClientStatementSpec statement_spec;
    VpsClientStatementMetadata statement_metadata;
    VpsError error;
    VpsClientStatus connect_status = VPS_CLIENT_INVALID_STATE;
    VpsClientStatus statement_status = VPS_CLIENT_INVALID_STATE;
    VpsClientStatus health_status = VPS_CLIENT_INVALID_STATE;
    VpsClientStatus metadata_status = VPS_CLIENT_INVALID_STATE;
    VpsClientStatus cancel_status = VPS_CLIENT_INVALID_STATE;
    VpsClientStatus fixture_status = VPS_CLIENT_OK;
    VpsClientStatus network_status = VPS_CLIENT_OK;
    VpsClientPollResult cancel_poll;
    VpsConnectionPool *pool = NULL;
    VpsConnectionLease lease = {0};
    VpsConnectionPoolConfig pool_config;
    VpsConnectionPoolKey pool_key;
    VpsIntegrationPoolContext pool_context;
    int arguments_initialized = 0;
    int config_initialized = 0;
    int identity_initialized = 0;
    int error_initialized = 0;
    int adapter_initialized = 0;
    int client_initialized = 0;
    int passed = 0;
    int fixture_requested = 0;
    int network_requested = 0;
    VpsErrorClass final_error_class = VPS_ERROR_CLASS_NONE;
    char final_sqlstate[6] = {0};
    if (!vps_async_environment(&environment)) {
        (void)printf(
            "async_connect_probe status=skipped reason=runtime_env_missing\n");
        return 77;
    }
    fixture_requested = getenv(VPS_ASYNC_ENV_FIXTURE) != NULL &&
                        strcmp(getenv(VPS_ASYNC_ENV_FIXTURE), "1") == 0;
    network_requested = getenv(VPS_ASYNC_ENV_NETWORK_LOSS) != NULL &&
                        strcmp(getenv(VPS_ASYNC_ENV_NETWORK_LOSS), "1") == 0;
    if (fixture_requested) fixture_status = VPS_CLIENT_INVALID_STATE;
    (void)memset(&log_capture, 0, sizeof(log_capture));
    (void)memset(&api_context, 0, sizeof(api_context));
    (void)memset(&error, 0, sizeof(error));
    (void)memset(&statement_metadata, 0, sizeof(statement_metadata));
    log_capture.environment = &environment;
    if (vps_allocator_system(&allocator) != VPS_MEMORY_OK ||
        vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG, vps_async_log_sink,
                        &log_capture) != VPS_LOG_OK ||
        vps_arguments_init(&arguments, &allocator,
                           vps_platform_current_operations(), &logger) !=
            VPS_ARGUMENTS_OK) goto cleanup;
    arguments_initialized = 1;
    if (!vps_async_arguments(&arguments, fixture_requested) ||
        vps_connection_config_init(&config, &allocator,
                                   vps_platform_current_operations(),
                                   &logger) != VPS_CONNECTION_STRING_OK) {
        goto cleanup;
    }
    config_initialized = 1;
    vps_async_configure(&config, &environment);
    tls_options.allow_explicit_disable =
        strcmp(environment.sslmode, "disable") == 0;
    if (vps_tls_policy_from_config(&config.config, &tls_options,
                                   &tls_policy) != VPS_TLS_OK ||
        vps_session_plan_init(&session_plan, &logger) != VPS_SESSION_OK ||
        vps_session_plan_build(&session_plan, &config, &arguments,
                               &session_options) != VPS_SESSION_OK ||
        vps_identity_init(&identity, &allocator, &logger) != VPS_IDENTITY_OK) {
        goto cleanup;
    }
    identity_initialized = 1;
    if (vps_identity_build(&identity, &config, &arguments,
                           &identity_options) != VPS_IDENTITY_OK ||
        vps_error_init(&error, &allocator) != VPS_MEMORY_OK) goto cleanup;
    error_initialized = 1;
    api_context.production = vps_libpq_client_default_api();
    api_context.finish_count = 0U;
    api = *api_context.production;
    api.context = &api_context;
    api.finish = vps_async_finish;
    (void)memset(&adapter_options, 0, sizeof(adapter_options));
    adapter_options.allocator = &allocator;
    adapter_options.platform_operations = vps_platform_current_operations();
    adapter_options.connection_config = &config;
    adapter_options.identity = &identity;
    adapter_options.tls_policy = &tls_policy;
    adapter_options.session_plan = &session_plan;
    adapter_options.logger = &logger;
    adapter_options.connect_timeout_ms = 10000U;
    adapter_options.wait_slice_ms = 50U;
    adapter_options.api = &api;
    if (vps_libpq_client_init(&adapter, &adapter_options) != VPS_CLIENT_OK) {
        goto cleanup;
    }
    adapter_initialized = 1;
    if (vps_libpq_client_make_operations(&adapter, &client_operations) !=
            VPS_CLIENT_OK ||
        vps_client_init(&client, &allocator, &client_operations, &adapter,
                        &logger) != VPS_CLIENT_OK) goto cleanup;
    client_initialized = 1;
    (void)memset(&pool_config, 0, sizeof(pool_config));
    pool_key.identity = identity.canonical.data;
    pool_key.identity_size = identity.canonical.size;
    pool_key.fingerprint = vps_identity_fingerprint(&identity);
    pool_key.credential_generation = identity.credential_generation;
    pool_key.configuration_generation = identity.configuration_generation;
    pool_key.read_only = fixture_requested ? 0 : 1;
    pool_context.client = &client;
    pool_context.error = &error;
    pool_config.allocator = allocator;
    pool_config.platform = vps_platform_current_operations();
    pool_config.logger = &logger;
    pool_config.key = pool_key;
    pool_config.callbacks.context = &pool_context;
    pool_config.callbacks.create = vps_integration_pool_create;
    pool_config.callbacks.validate = vps_integration_pool_validate;
    pool_config.callbacks.reset = vps_integration_pool_reset;
    pool_config.callbacks.destroy = vps_integration_pool_destroy;
    pool_config.maximum_size = 1U;
    pool_config.maximum_waiters = 1U;
    pool_config.wait_slice_ms = 50U;
    pool_config.idle_validation_ms = 0U;
    connect_status =
        vps_connection_pool_create(&pool_config, &pool) ==
                VPS_CONNECTION_POOL_OK &&
            vps_connection_pool_acquire(pool, &pool_key, 10000U, NULL, NULL,
                                        &lease) == VPS_CONNECTION_POOL_OK
        ? VPS_CLIENT_OK
        : VPS_CLIENT_BACKEND_ERROR;
    connection = (VpsClientConnection *)lease.connection;
    if (connect_status == VPS_CLIENT_OK) {
        metadata_status = vps_async_metadata_probe(connection, &allocator,
                                                   &logger, &error);
        if (fixture_requested) {
            fixture_status = vps_async_fixture_bootstrap_and_verify(
                connection, &error);
        }
    }
    if (connect_status == VPS_CLIENT_OK &&
        metadata_status == VPS_CLIENT_OK && fixture_status == VPS_CLIENT_OK) {
        static const char query[] =
            "SELECT $1::pg_catalog.int4 WHERE false";
        static const char parameter_value[] = "7";
        (void)memset(&parameter, 0, sizeof(parameter));
        parameter.value = parameter_value;
        parameter.length = sizeof(parameter_value) - 1U;
        parameter.type_oid = 23U;
        parameter.format = VPS_CLIENT_VALUE_TEXT;
        result_field.type_oid = 23U;
        result_field.format = VPS_CLIENT_VALUE_TEXT;
        (void)memset(&statement_spec, 0, sizeof(statement_spec));
        statement_spec.query = query;
        statement_spec.query_length = sizeof(query) - 1U;
        statement_spec.parameters = &parameter;
        statement_spec.parameter_count = 1U;
        statement_spec.result_fields = &result_field;
        statement_spec.result_field_count = 1U;
        statement_spec.timeout_ms = 5000U;
        statement_spec.prepare = 1;
        statement_spec.single_row = 1;
        statement_status = vps_client_statement_open(
            connection, &statement_spec, &statement, &error);
        if (statement_status == VPS_CLIENT_OK) {
            statement_status = vps_client_statement_start(
                statement, VPS_CLIENT_OPERATION_PREPARE, &error);
        }
        if (statement_status == VPS_CLIENT_OK) {
            statement_status = vps_async_drive_statement(statement, &error);
        }
        if (statement_status == VPS_CLIENT_OK) {
            statement_status = vps_client_statement_metadata(
                statement, &statement_metadata, &error);
        }
        if (statement_status == VPS_CLIENT_OK &&
            (statement_metadata.parameter_count != 1U ||
             statement_metadata.result_field_count != 1U ||
             !statement_metadata.described)) {
            statement_status = VPS_CLIENT_BACKEND_ERROR;
        }
        if (statement_status == VPS_CLIENT_OK) {
            statement_status = vps_client_statement_start(
                statement, VPS_CLIENT_OPERATION_EXECUTE, &error);
        }
        if (statement_status == VPS_CLIENT_OK) {
            statement_status = vps_async_drive_statement(statement, &error);
        }
        if (statement_status == VPS_CLIENT_OK) {
            statement_status = vps_client_statement_start(
                statement, VPS_CLIENT_OPERATION_FETCH, &error);
        }
        if (statement_status == VPS_CLIENT_OK) {
            statement_status = vps_async_drive_statement(statement, &error);
        }
        if (statement_status == VPS_CLIENT_OK) {
            statement_status = vps_client_statement_close(&statement);
        }
        if (statement_status == VPS_CLIENT_OK) {
            static const char cancel_query[] =
                "SELECT pg_catalog.pg_sleep(10)";
            result_field.type_oid = 2278U;
            result_field.format = VPS_CLIENT_VALUE_TEXT;
            (void)memset(&statement_spec, 0, sizeof(statement_spec));
            statement_spec.query = cancel_query;
            statement_spec.query_length = sizeof(cancel_query) - 1U;
            statement_spec.result_fields = &result_field;
            statement_spec.result_field_count = 1U;
            statement_spec.timeout_ms = 15000U;
            cancel_status = vps_client_statement_open(
                connection, &statement_spec, &statement, &error);
        }
        if (cancel_status == VPS_CLIENT_OK) {
            cancel_status = vps_client_statement_start(
                statement, VPS_CLIENT_OPERATION_EXECUTE, &error);
        }
        if (cancel_status == VPS_CLIENT_OK) {
            cancel_status = vps_client_statement_poll(
                statement, &cancel_poll, &error);
            if (cancel_status == VPS_CLIENT_OK &&
                cancel_poll.outcome != VPS_CLIENT_POLL_WAIT) {
                cancel_status = VPS_CLIENT_BACKEND_ERROR;
            }
            if (cancel_status == VPS_CLIENT_OK &&
                !vps_cancel_probe_settle(100U)) {
                cancel_status = VPS_CLIENT_BACKEND_ERROR;
            }
        }
        if (cancel_status == VPS_CLIENT_OK) {
            cancel_status = vps_client_statement_start(
                statement, VPS_CLIENT_OPERATION_CANCEL, &error);
        }
        if (cancel_status == VPS_CLIENT_OK) {
            cancel_status = vps_async_drive_statement(statement, &error);
        }
        if (cancel_status == VPS_CLIENT_OK &&
            error.error_class == VPS_ERROR_CLASS_CANCEL &&
            strcmp(error.sqlstate, "57014") == 0) {
            cancel_status = vps_client_statement_close(&statement);
            vps_error_reset(&error);
        } else if (cancel_status == VPS_CLIENT_OK) {
            cancel_status = VPS_CLIENT_BACKEND_ERROR;
        }
        if (statement_status == VPS_CLIENT_OK &&
            cancel_status == VPS_CLIENT_OK &&
            vps_connection_lease_release(
                &lease, VPS_CONNECTION_LEASE_CLEAN) ==
                VPS_CONNECTION_POOL_OK &&
            vps_connection_pool_acquire(pool, &pool_key, 10000U, NULL, NULL,
                                        &lease) == VPS_CONNECTION_POOL_OK &&
            lease.connection == connection &&
            vps_connection_lease_release(
                &lease, VPS_CONNECTION_LEASE_CLEAN) ==
                VPS_CONNECTION_POOL_OK) {
            health_status = VPS_CLIENT_OK;
        }
    }
    if (network_requested && health_status == VPS_CLIENT_OK) {
        network_status = VPS_CLIENT_BACKEND_ERROR;
        if (vps_connection_pool_acquire(pool, &pool_key, 10000U, NULL, NULL,
                                        &lease) == VPS_CONNECTION_POOL_OK) {
            connection = (VpsClientConnection *)lease.connection;
            if (vps_async_network_loss_before_row(connection, &error) &&
                vps_connection_lease_release(
                    &lease, VPS_CONNECTION_LEASE_DIRTY) ==
                    VPS_CONNECTION_POOL_OK) {
                connection = NULL;
                vps_error_reset(&error);
                if (vps_connection_pool_acquire(
                        pool, &pool_key, 10000U, NULL, NULL, &lease) ==
                    VPS_CONNECTION_POOL_OK) {
                    connection = (VpsClientConnection *)lease.connection;
                    if (vps_async_network_loss_after_row(connection, &error) &&
                        vps_connection_lease_release(
                            &lease, VPS_CONNECTION_LEASE_DIRTY) ==
                            VPS_CONNECTION_POOL_OK) {
                        connection = NULL;
                        vps_error_reset(&error);
                        network_status = VPS_CLIENT_OK;
                    }
                }
            }
        }
    }
    passed = connect_status == VPS_CLIENT_OK &&
             statement_status == VPS_CLIENT_OK &&
             cancel_status == VPS_CLIENT_OK &&
             health_status == VPS_CLIENT_OK &&
             metadata_status == VPS_CLIENT_OK &&
             fixture_status == VPS_CLIENT_OK &&
             network_status == VPS_CLIENT_OK &&
             !log_capture.sensitive_value_seen;
    final_error_class = error.error_class;
    (void)memcpy(final_sqlstate, error.sqlstate, sizeof(error.sqlstate));

cleanup:
    if (statement != NULL) (void)vps_client_statement_close(&statement);
    if (lease.pool != NULL) {
        (void)vps_connection_lease_release(&lease,
                                           VPS_CONNECTION_LEASE_DIRTY);
    }
    if (pool != NULL) (void)vps_connection_pool_destroy(&pool);
    if (client_initialized) (void)vps_client_cleanup(&client);
    if (adapter_initialized) (void)vps_libpq_client_cleanup(&adapter);
    if (error_initialized) vps_error_reset(&error);
    if (identity_initialized) vps_identity_cleanup(&identity);
    if (config_initialized) (void)vps_connection_config_cleanup(&config);
    if (arguments_initialized) (void)vps_arguments_reset(&arguments);
    if (passed && api_context.finish_count !=
                      (network_requested ? 2U : 1U)) passed = 0;
    (void)printf(
        "async_connect_probe status=%s result=%s metadata=%s fixture=%s statement=%s cancel=%s health=%s network=%s error_class=%s sqlstate=%s phase=%s outcome=%s finish_count=%u log_events=%u\n",
        passed ? "passed" : "failed",
        vps_client_status_name(connect_status),
        vps_client_status_name(metadata_status),
        vps_client_status_name(fixture_status),
        vps_client_status_name(statement_status),
        vps_client_status_name(cancel_status),
        vps_client_status_name(health_status),
        vps_client_status_name(network_status),
        vps_error_class_name(final_error_class),
        final_sqlstate[0] == '\0' ? "none" : final_sqlstate,
        log_capture.failure_phase[0] == '\0' ? "none"
                                              : log_capture.failure_phase,
        log_capture.failure_status[0] == '\0' ? "none"
                                               : log_capture.failure_status,
        (unsigned int)api_context.finish_count,
        (unsigned int)log_capture.event_count);
    return passed ? 0 : 1;
}
