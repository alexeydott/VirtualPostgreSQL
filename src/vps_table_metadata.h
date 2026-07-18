#ifndef VPS_TABLE_METADATA_H
#define VPS_TABLE_METADATA_H

#include "vps_client.h"
#include "vps_metadata.h"
#include "vps_type_registry.h"

typedef struct VpsTableMetadata {
    VpsRelationMetadata relation;
    VpsColumnSet columns;
    VpsKeyMetadata key;
    VpsRelationPolicyMetadata policy;
    VpsTypeRegistry type_registry;
    int initialized_relation;
    int initialized_columns;
    int initialized_registry;
    int loaded;
} VpsTableMetadata;

VpsMetadataResult vps_table_metadata_load(
    VpsTableMetadata *metadata,
    VpsClientConnection *connection,
    const VpsAllocator *allocator,
    const char *schema,
    size_t schema_length,
    const char *relation,
    size_t relation_length,
    VpsLogger *logger,
    VpsError *error);
VpsClientStatus vps_catalog_metadata_fetch(
    VpsClientConnection *connection,
    VpsCatalogQuery query,
    const VpsClientParameterView *parameters,
    size_t parameter_count,
    VpsMetadataRowSet *rowset,
    VpsError *error);
void vps_table_metadata_reset(VpsTableMetadata *metadata);

#endif
