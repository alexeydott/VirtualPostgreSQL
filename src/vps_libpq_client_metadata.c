#include "vps_libpq_client_metadata.h"

#include <string.h>

VpsMetadataResult vps_libpq_metadata_statement_init(
    VpsLibpqMetadataStatement *metadata_statement,
    VpsCatalogQuery query,
    const VpsClientParameterView *parameters,
    size_t parameter_count,
    uint64_t timeout_ms)
{
    VpsCatalogQuerySpec catalog;
    size_t index;
    if (metadata_statement == NULL ||
        vps_metadata_catalog_query_spec(query, &catalog) != VPS_METADATA_OK ||
        parameter_count != catalog.parameter_count ||
        (parameter_count != 0U && parameters == NULL) || timeout_ms == 0U ||
        timeout_ms > VPS_CLIENT_MAX_STATEMENT_TIMEOUT_MS)
        return VPS_METADATA_INVALID_ARGUMENT;
    (void)memset(metadata_statement, 0, sizeof(*metadata_statement));
    for (index = 0U; index < parameter_count; ++index) {
        if (parameters[index].format != VPS_CLIENT_VALUE_TEXT ||
            parameters[index].is_null || parameters[index].value == NULL ||
            parameters[index].length == 0U ||
            parameters[index].length > VPS_METADATA_NAME_MAX_BYTES)
            return VPS_METADATA_INVALID_ARGUMENT;
        metadata_statement->parameters[index] = parameters[index];
    }
    for (index = 0U; index < catalog.result_field_count; ++index) {
        metadata_statement->result_fields[index].type_oid =
            VPS_METADATA_TEXT_OID;
        metadata_statement->result_fields[index].format =
            VPS_CLIENT_VALUE_TEXT;
    }
    metadata_statement->statement.query = catalog.sql;
    metadata_statement->statement.query_length = catalog.sql_length;
    metadata_statement->statement.parameters = metadata_statement->parameters;
    metadata_statement->statement.parameter_count = parameter_count;
    metadata_statement->statement.result_fields =
        metadata_statement->result_fields;
    metadata_statement->statement.result_field_count =
        catalog.result_field_count;
    metadata_statement->statement.timeout_ms = timeout_ms;
    metadata_statement->statement.prepare = 0;
    metadata_statement->statement.single_row = 1;
    return VPS_METADATA_OK;
}
