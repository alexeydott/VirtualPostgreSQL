#ifndef VPS_SCHEMA_FINGERPRINT_H
#define VPS_SCHEMA_FINGERPRINT_H

#include "vps_metadata.h"
#include "vps_type_registry.h"

#include <stdint.h>

#define VPS_SCHEMA_FINGERPRINT_VERSION UINT32_C(1)
#define VPS_SCHEMA_FINGERPRINT_BYTES 32U
#define VPS_SCHEMA_FINGERPRINT_HEX_LENGTH 64U
#define VPS_SCHEMA_FINGERPRINT_HEX_SIZE 65U

typedef enum VpsSchemaChangeClass {
    VPS_SCHEMA_CHANGE_NONE = 0,
    VPS_SCHEMA_CHANGE_RELATION_IDENTITY = 1,
    VPS_SCHEMA_CHANGE_COLUMN_LAYOUT = 2,
    VPS_SCHEMA_CHANGE_KEY = 3,
    VPS_SCHEMA_CHANGE_RELATION_POLICY = 4,
    VPS_SCHEMA_CHANGE_CODEC_REGISTRY = 5,
    VPS_SCHEMA_CHANGE_SPATIAL = 6,
    VPS_SCHEMA_CHANGE_UNKNOWN = 7
} VpsSchemaChangeClass;

typedef struct VpsSchemaSpatialMetadata {
    uint32_t metadata_version;
    uint32_t type_oid;
    uint32_t extension_namespace_oid;
    int32_t srid;
    uint32_t dimensions;
    uint32_t format;
} VpsSchemaSpatialMetadata;

typedef struct VpsSchemaFingerprintInput {
    const VpsRelationMetadata *relation;
    const VpsColumnSet *columns;
    const VpsKeyMetadata *key;
    const VpsRelationPolicyMetadata *policy;
    const VpsTypeRegistry *type_registry;
    VpsSchemaSpatialMetadata spatial;
} VpsSchemaFingerprintInput;

typedef struct VpsSchemaFingerprint {
    uint32_t version;
    unsigned char bytes[VPS_SCHEMA_FINGERPRINT_BYTES];
    char hex[VPS_SCHEMA_FINGERPRINT_HEX_SIZE];
    uint64_t relation_hash;
    uint64_t layout_hash;
    uint64_t key_hash;
    uint64_t policy_hash;
    uint64_t codec_hash;
    uint64_t spatial_hash;
    size_t input_column_count;
} VpsSchemaFingerprint;

VpsMetadataResult vps_schema_fingerprint_build(
    const VpsSchemaFingerprintInput *input,
    VpsLogger *logger,
    VpsSchemaFingerprint *fingerprint);
VpsSchemaChangeClass vps_schema_fingerprint_compare(
    const VpsSchemaFingerprint *previous,
    const VpsSchemaFingerprint *current);
const char *vps_schema_change_class_name(VpsSchemaChangeClass change_class);

#endif
