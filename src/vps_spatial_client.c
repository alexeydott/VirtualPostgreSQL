#include "vps_spatial_client.h"

#include "vps_libpq_client_metadata.h"

#include <string.h>

#define VPS_SPATIAL_CLIENT_TIMEOUT_MS 10000U
#define VPS_SPATIAL_CLIENT_DRIVE_LIMIT 65536U

typedef struct VpsSpatialClientRow {
    VpsClientColumnView columns[15];
} VpsSpatialClientRow;

static int vps_spatial_input_null(void *context, size_t row, size_t field)
{
    (void)row;
    return ((VpsSpatialClientRow *)context)->columns[field].is_null;
}

static const void *vps_spatial_input_value(void *context, size_t row,
                                           size_t field)
{
    (void)row;
    return ((VpsSpatialClientRow *)context)->columns[field].data;
}

static size_t vps_spatial_input_length(void *context, size_t row,
                                       size_t field)
{
    (void)row;
    return ((VpsSpatialClientRow *)context)->columns[field].length;
}

static VpsClientStatus vps_spatial_drive(VpsClientStatement *statement,
                                         VpsError *error)
{
    size_t attempt;
    for (attempt = 0U; attempt < VPS_SPATIAL_CLIENT_DRIVE_LIMIT; ++attempt) {
        VpsClientPollResult poll;
        VpsClientStatus status = vps_client_statement_poll(statement, &poll,
                                                            error);
        if (status != VPS_CLIENT_OK) return status;
        if (poll.outcome == VPS_CLIENT_POLL_COMPLETE ||
            poll.outcome == VPS_CLIENT_POLL_ROW_READY) return VPS_CLIENT_OK;
        if (poll.outcome != VPS_CLIENT_POLL_WAIT)
            return VPS_CLIENT_BACKEND_ERROR;
        status = vps_client_statement_wait(statement, &poll.wait, error);
        if (status != VPS_CLIENT_OK) return status;
    }
    return VPS_CLIENT_LIMIT_EXCEEDED;
}

VpsSpatialResult vps_spatial_capabilities_load(
    VpsSpatialCapabilities *capabilities, VpsClientConnection *connection,
    const VpsAllocator *allocator, VpsLogger *logger, VpsError *error)
{
    static const char extension[] = "postgis";
    VpsClientParameterView parameter;
    VpsLibpqMetadataStatement metadata;
    VpsClientStatement *statement = NULL;
    VpsMetadataRowSet rows;
    VpsSpatialClientRow row;
    VpsMetadataInput input;
    VpsClientStatus status;
    VpsSpatialResult result = VPS_SPATIAL_INVALID_RESULT;
    size_t row_count = 0U;
    (void)memset(&parameter, 0, sizeof(parameter));
    (void)memset(&rows, 0, sizeof(rows));
    (void)memset(&row, 0, sizeof(row));
    (void)memset(&input, 0, sizeof(input));
    if (capabilities == NULL || connection == NULL ||
        !vps_allocator_is_valid(allocator))
        return VPS_SPATIAL_INVALID_ARGUMENT;
    result = vps_spatial_capabilities_init(capabilities, allocator, logger);
    if (result != VPS_SPATIAL_OK) return result;
    if (vps_metadata_rowset_init(&rows, allocator, logger) != VPS_METADATA_OK) {
        vps_spatial_capabilities_reset(capabilities);
        return VPS_SPATIAL_OUT_OF_MEMORY;
    }
    result = VPS_SPATIAL_INVALID_RESULT;
    parameter.value = extension;
    parameter.length = sizeof(extension) - 1U;
    parameter.type_oid = VPS_METADATA_NAME_OID;
    parameter.format = VPS_CLIENT_VALUE_TEXT;
    if (vps_libpq_metadata_statement_init(
            &metadata, VPS_CATALOG_QUERY_POSTGIS, &parameter, 1U,
            VPS_SPATIAL_CLIENT_TIMEOUT_MS) != VPS_METADATA_OK) goto cleanup;
    input.context = &row;
    input.field_count = 15U;
    input.is_null = vps_spatial_input_null;
    input.value = vps_spatial_input_value;
    input.length = vps_spatial_input_length;
    status = vps_client_statement_open(connection, &metadata.statement,
                                       &statement, error);
    if (status == VPS_CLIENT_OK)
        status = vps_client_statement_start(statement,
                                            VPS_CLIENT_OPERATION_EXECUTE,
                                            error);
    if (status == VPS_CLIENT_OK) status = vps_spatial_drive(statement, error);
    while (status == VPS_CLIENT_OK &&
           vps_client_statement_state(statement) !=
               VPS_CLIENT_STATEMENT_COMPLETE) {
        const VpsClientRowView *current = NULL;
        size_t field;
        status = vps_client_statement_start(statement,
                                            VPS_CLIENT_OPERATION_FETCH,
                                            error);
        if (status == VPS_CLIENT_OK) status = vps_spatial_drive(statement, error);
        if (status != VPS_CLIENT_OK ||
            vps_client_statement_state(statement) ==
                VPS_CLIENT_STATEMENT_COMPLETE) break;
        status = vps_client_statement_current_row(statement, &current, error);
        if (status == VPS_CLIENT_OK &&
            vps_client_row_column_count(current) != 15U)
            status = VPS_CLIENT_BACKEND_ERROR;
        for (field = 0U; status == VPS_CLIENT_OK && field < 15U; ++field)
            status = vps_client_row_column(current, field,
                                           &row.columns[field], error);
        input.row_count = 1U;
        if (status == VPS_CLIENT_OK &&
            vps_metadata_rowset_append(&rows, VPS_CATALOG_QUERY_POSTGIS,
                                       &input) != VPS_METADATA_OK)
            status = VPS_CLIENT_BACKEND_ERROR;
        if (status == VPS_CLIENT_OK) {
            row_count += 1U;
            status = vps_client_statement_row_consumed(statement, error);
        }
    }
    if (status != VPS_CLIENT_OK || row_count > 1U) goto cleanup;
    if (row_count == 0U) {
        input.row_count = 0U;
        if (vps_metadata_rowset_copy(&rows, VPS_CATALOG_QUERY_POSTGIS,
                                     &input) != VPS_METADATA_OK) goto cleanup;
    }
    result = vps_spatial_capabilities_resolve(capabilities, &rows);
cleanup:
    if (statement != NULL) (void)vps_client_statement_close(&statement);
    vps_metadata_rowset_reset(&rows);
    if (result != VPS_SPATIAL_OK && result != VPS_SPATIAL_NOT_AVAILABLE)
        vps_spatial_capabilities_reset(capabilities);
    return result;
}
