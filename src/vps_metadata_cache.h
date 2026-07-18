#ifndef VPS_METADATA_CACHE_H
#define VPS_METADATA_CACHE_H

#include "vps_memory.h"
#include "vps_error.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_METADATA_CACHE_VERSION UINT32_C(1)
#define VPS_METADATA_CACHE_MAX_BYTES (1024U * 1024U)
#define VPS_METADATA_CACHE_MAX_FIELDS 64U
#define VPS_METADATA_CACHE_MAX_KEYS 32U

typedef enum VpsMetadataCacheResult {
    VPS_METADATA_CACHE_OK = 0,
    VPS_METADATA_CACHE_INVALID_ARGUMENT = 1,
    VPS_METADATA_CACHE_INVALID_FORMAT = 2,
    VPS_METADATA_CACHE_LIMIT_EXCEEDED = 3,
    VPS_METADATA_CACHE_OUT_OF_MEMORY = 4,
    VPS_METADATA_CACHE_INCOMPATIBLE = 5
} VpsMetadataCacheResult;

typedef enum VpsMetadataDrift {
    VPS_METADATA_DRIFT_NONE = 0,
    VPS_METADATA_DRIFT_REFRESHABLE = 1,
    VPS_METADATA_DRIFT_INCOMPATIBLE = 2
} VpsMetadataDrift;

typedef struct VpsMetadataCacheString {
    size_t offset;
    size_t length;
} VpsMetadataCacheString;

typedef struct VpsMetadataCacheField {
    VpsMetadataCacheString name;
    uint32_t type_oid;
    int32_t type_modifier;
    uint32_t origin_relation_oid;
    int32_t origin_attribute_number;
    uint32_t spatial_srid;
    uint32_t collation_oid;
    uint64_t policy_fingerprint;
    uint8_t spatial_kind;
    uint8_t spatial_type;
    uint8_t spatial_dimensions;
} VpsMetadataCacheField;

typedef struct VpsMetadataSnapshot {
    VpsAllocator allocator;
    VpsBuffer text;
    VpsMetadataCacheField fields[VPS_METADATA_CACHE_MAX_FIELDS];
    uint16_t key_columns[VPS_METADATA_CACHE_MAX_KEYS];
    size_t field_count;
    size_t visible_count;
    size_t key_count;
    uint64_t source_fingerprint;
    uint64_t layout_fingerprint;
    uint64_t captured_at_ms;
    uint64_t validated_at_ms;
    uint64_t configuration_generation;
    uint64_t connection_identity_hash;
    uint64_t relation_policy_fingerprint;
    uint32_t relation_oid;
    uint32_t spatial_namespace_oid;
    uint32_t spatial_geometry_oid;
    uint32_t spatial_geography_oid;
    uint32_t spatial_flags;
    uint32_t spatial_format;
    int initialized;
} VpsMetadataSnapshot;

VpsMetadataCacheResult vps_metadata_snapshot_init(
    VpsMetadataSnapshot *snapshot, const VpsAllocator *allocator);
VpsMetadataCacheResult vps_metadata_snapshot_add_field(
    VpsMetadataSnapshot *snapshot, const char *name, size_t name_length,
    const VpsMetadataCacheField *field);
VpsMetadataCacheResult vps_metadata_snapshot_encode(
    const VpsMetadataSnapshot *snapshot, VpsBuffer *encoded);
VpsMetadataCacheResult vps_metadata_snapshot_decode(
    VpsMetadataSnapshot *snapshot, const VpsAllocator *allocator,
    const void *encoded, size_t encoded_size);
VpsMetadataCacheResult vps_metadata_snapshot_validate(
    const VpsMetadataSnapshot *snapshot);
VpsMetadataDrift vps_metadata_snapshot_compare(
    const VpsMetadataSnapshot *stored, const VpsMetadataSnapshot *live);
int vps_metadata_cache_fallback_allowed(VpsErrorClass error_class);
const char *vps_metadata_snapshot_field_name(
    const VpsMetadataSnapshot *snapshot, size_t index, size_t *length);
void vps_metadata_snapshot_reset(VpsMetadataSnapshot *snapshot);

#endif
