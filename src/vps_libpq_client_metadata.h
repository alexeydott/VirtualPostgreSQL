#ifndef VPS_LIBPQ_CLIENT_METADATA_H
#define VPS_LIBPQ_CLIENT_METADATA_H

#include "vps_client.h"
#include "vps_metadata.h"

typedef struct VpsLibpqMetadataStatement {
    VpsClientStatementSpec statement;
    VpsClientParameterView parameters[3];
    VpsClientResultFieldExpectation result_fields[VPS_METADATA_MAX_FIELDS];
} VpsLibpqMetadataStatement;

VpsMetadataResult vps_libpq_metadata_statement_init(
    VpsLibpqMetadataStatement *metadata_statement,
    VpsCatalogQuery query,
    const VpsClientParameterView *parameters,
    size_t parameter_count,
    uint64_t timeout_ms);

#endif
