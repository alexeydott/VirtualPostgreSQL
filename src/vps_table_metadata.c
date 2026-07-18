#include "vps_table_metadata.h"

#include "vps_libpq_client_metadata.h"

#include <stdio.h>
#include <string.h>

#define VPS_TABLE_METADATA_DRIVE_LIMIT 65536U
#define VPS_TABLE_METADATA_TIMEOUT_MS 10000U

typedef struct VpsMetadataRowView {
    VpsClientColumnView columns[VPS_METADATA_MAX_FIELDS];
} VpsMetadataRowView;

static int vps_metadata_input_is_null(void *context,
                                      size_t row,
                                      size_t field)
{
    VpsMetadataRowView *view = (VpsMetadataRowView *)context;
    (void)row;
    return view->columns[field].is_null;
}

static const void *vps_metadata_input_value(void *context,
                                            size_t row,
                                            size_t field)
{
    VpsMetadataRowView *view = (VpsMetadataRowView *)context;
    (void)row;
    return view->columns[field].data;
}

static size_t vps_metadata_input_length(void *context,
                                        size_t row,
                                        size_t field)
{
    VpsMetadataRowView *view = (VpsMetadataRowView *)context;
    (void)row;
    return view->columns[field].length;
}

static VpsClientStatus vps_metadata_drive(VpsClientStatement *statement,
                                          VpsError *error)
{
    size_t attempt;
    for (attempt = 0U; attempt < VPS_TABLE_METADATA_DRIVE_LIMIT; ++attempt) {
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

static VpsClientStatus vps_metadata_fetch(
    VpsClientConnection *connection,
    VpsCatalogQuery query,
    const VpsClientParameterView *parameters,
    size_t parameter_count,
    VpsMetadataRowSet *rowset,
    VpsError *error)
{
    VpsLibpqMetadataStatement spec;
    VpsClientStatement *statement = NULL;
    VpsMetadataRowView row_view;
    VpsMetadataInput input;
    VpsClientStatus status;
    size_t rows = 0U;
    (void)memset(&row_view, 0, sizeof(row_view));
    (void)memset(&input, 0, sizeof(input));
    if (vps_libpq_metadata_statement_init(
            &spec, query, parameters, parameter_count,
            VPS_TABLE_METADATA_TIMEOUT_MS) != VPS_METADATA_OK)
        return VPS_CLIENT_INVALID_ARGUMENT;
    input.context = &row_view;
    input.field_count = spec.statement.result_field_count;
    input.is_null = vps_metadata_input_is_null;
    input.value = vps_metadata_input_value;
    input.length = vps_metadata_input_length;
    status = vps_client_statement_open(connection, &spec.statement,
                                       &statement, error);
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_EXECUTE, error);
    if (status == VPS_CLIENT_OK) status = vps_metadata_drive(statement, error);
    while (status == VPS_CLIENT_OK &&
           vps_client_statement_state(statement) !=
               VPS_CLIENT_STATEMENT_COMPLETE) {
        const VpsClientRowView *row = NULL;
        size_t field;
        status = vps_client_statement_start(
            statement, VPS_CLIENT_OPERATION_FETCH, error);
        if (status == VPS_CLIENT_OK)
            status = vps_metadata_drive(statement, error);
        if (status != VPS_CLIENT_OK ||
            vps_client_statement_state(statement) ==
                VPS_CLIENT_STATEMENT_COMPLETE)
            break;
        status = vps_client_statement_current_row(statement, &row, error);
        if (status == VPS_CLIENT_OK &&
            vps_client_row_column_count(row) != input.field_count)
            status = VPS_CLIENT_BACKEND_ERROR;
        for (field = 0U; status == VPS_CLIENT_OK &&
                         field < input.field_count; ++field)
            status = vps_client_row_column(row, field,
                                           &row_view.columns[field], error);
        input.row_count = 1U;
        if (status == VPS_CLIENT_OK &&
            vps_metadata_rowset_append(rowset, query, &input) !=
                VPS_METADATA_OK)
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
    if (statement != NULL &&
        vps_client_statement_close(&statement) != VPS_CLIENT_OK &&
        status == VPS_CLIENT_OK)
        status = VPS_CLIENT_BACKEND_ERROR;
    return status;
}

static void vps_metadata_text_parameter(VpsClientParameterView *parameter,
                                        const void *value,
                                        size_t length)
{
    (void)memset(parameter, 0, sizeof(*parameter));
    parameter->value = value;
    parameter->length = length;
    parameter->type_oid = VPS_METADATA_TEXT_OID;
    parameter->format = VPS_CLIENT_VALUE_TEXT;
}

VpsMetadataResult vps_table_metadata_load(
    VpsTableMetadata *metadata,
    VpsClientConnection *connection,
    const VpsAllocator *allocator,
    const char *schema,
    size_t schema_length,
    const char *relation,
    size_t relation_length,
    VpsLogger *logger,
    VpsError *error)
{
    VpsMetadataRowSet rowset;
    VpsClientParameterView parameters[2];
    char relation_oid[16];
    int relation_oid_length;
    VpsMetadataResult result = VPS_METADATA_INVALID_RESULT;
    if (metadata == NULL || connection == NULL || allocator == NULL ||
        schema == NULL || schema_length == 0U || relation == NULL ||
        relation_length == 0U || metadata->loaded)
        return VPS_METADATA_INVALID_ARGUMENT;
    (void)memset(metadata, 0, sizeof(*metadata));
    (void)memset(&rowset, 0, sizeof(rowset));
    if (vps_relation_metadata_init(&metadata->relation, allocator, logger) !=
            VPS_METADATA_OK)
        goto cleanup;
    metadata->initialized_relation = 1;
    if (vps_column_set_init(&metadata->columns, allocator, logger) !=
            VPS_METADATA_OK)
        goto cleanup;
    metadata->initialized_columns = 1;
    if (vps_type_registry_init(&metadata->type_registry, logger) !=
            VPS_METADATA_OK)
        goto cleanup;
    metadata->initialized_registry = 1;
    if (vps_metadata_rowset_init(&rowset, allocator, logger) != VPS_METADATA_OK)
        goto cleanup;
    vps_metadata_text_parameter(&parameters[0], schema, schema_length);
    vps_metadata_text_parameter(&parameters[1], relation, relation_length);
    if (vps_metadata_fetch(connection, VPS_CATALOG_QUERY_RELATION,
                           parameters, 2U, &rowset, error) != VPS_CLIENT_OK ||
        vps_relation_metadata_resolve(&metadata->relation, &rowset,
                                      schema, schema_length,
                                      relation, relation_length) !=
            VPS_METADATA_OK)
        goto cleanup;
    relation_oid_length = snprintf(relation_oid, sizeof(relation_oid), "%u",
                                   metadata->relation.relation_oid);
    if (relation_oid_length <= 0 ||
        (size_t)relation_oid_length >= sizeof(relation_oid))
        goto cleanup;
    vps_metadata_text_parameter(&parameters[0], relation_oid,
                                (size_t)relation_oid_length);
    vps_metadata_rowset_reset(&rowset);
    if (vps_metadata_rowset_init(&rowset, allocator, logger) != VPS_METADATA_OK ||
        vps_metadata_fetch(connection, VPS_CATALOG_QUERY_COLUMNS,
                           parameters, 1U, &rowset, error) != VPS_CLIENT_OK ||
        vps_column_set_build(&metadata->columns, &rowset) != VPS_METADATA_OK)
        goto cleanup;
    vps_metadata_rowset_reset(&rowset);
    if (vps_metadata_rowset_init(&rowset, allocator, logger) != VPS_METADATA_OK ||
        vps_metadata_fetch(connection, VPS_CATALOG_QUERY_KEYS,
                           parameters, 1U, &rowset, error) != VPS_CLIENT_OK ||
        vps_key_discover(&rowset, &metadata->columns, NULL, 0U, 0,
                         logger, &metadata->key) != VPS_METADATA_OK)
        goto cleanup;
    vps_metadata_rowset_reset(&rowset);
    if (vps_metadata_rowset_init(&rowset, allocator, logger) != VPS_METADATA_OK ||
        vps_metadata_fetch(connection, VPS_CATALOG_QUERY_RELATION_POLICY,
                           parameters, 1U, &rowset, error) != VPS_CLIENT_OK ||
        vps_relation_policy_build(&metadata->relation, &metadata->key,
                                  &rowset, logger, &metadata->policy) !=
            VPS_METADATA_OK)
        goto cleanup;
    metadata->loaded = 1;
    result = VPS_METADATA_OK;
cleanup:
    vps_metadata_rowset_reset(&rowset);
    if (result != VPS_METADATA_OK) vps_table_metadata_reset(metadata);
    return result;
}

void vps_table_metadata_reset(VpsTableMetadata *metadata)
{
    if (metadata == NULL) return;
    if (metadata->initialized_columns)
        vps_column_set_reset(&metadata->columns);
    if (metadata->initialized_relation)
        vps_relation_metadata_reset(&metadata->relation);
    (void)memset(metadata, 0, sizeof(*metadata));
}
