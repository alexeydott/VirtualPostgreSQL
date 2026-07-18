#ifndef VPS_QUERY_FINGERPRINT_H
#define VPS_QUERY_FINGERPRINT_H

#include "vps_query_metadata.h"

#include <stdint.h>

#define VPS_QUERY_FINGERPRINT_VERSION UINT32_C(1)
#define VPS_QUERY_FINGERPRINT_BYTES 32U
#define VPS_QUERY_FINGERPRINT_HEX_LENGTH 64U
#define VPS_QUERY_FINGERPRINT_HEX_SIZE 65U

typedef enum VpsQueryChangeClass {
    VPS_QUERY_CHANGE_NONE = 0,
    VPS_QUERY_CHANGE_SOURCE = 1,
    VPS_QUERY_CHANGE_PROFILE = 2,
    VPS_QUERY_CHANGE_LAYOUT = 3,
    VPS_QUERY_CHANGE_KEY = 4,
    VPS_QUERY_CHANGE_INDEXES = 5,
    VPS_QUERY_CHANGE_POLICY = 6,
    VPS_QUERY_CHANGE_WRAPPER = 7,
    VPS_QUERY_CHANGE_UNKNOWN = 8
} VpsQueryChangeClass;

typedef struct VpsQueryFingerprintInput {
    uint64_t normalized_query_hash;
    uint64_t profile_version;
    uint64_t profile_generation;
    const VpsQueryResultMetadata *metadata;
    uint32_t wrapper_format_version;
    uint32_t codec_registry_version;
} VpsQueryFingerprintInput;

typedef struct VpsQueryFingerprint {
    uint32_t version;
    unsigned char bytes[VPS_QUERY_FINGERPRINT_BYTES];
    char hex[VPS_QUERY_FINGERPRINT_HEX_SIZE];
    uint64_t source_hash;
    uint64_t profile_hash;
    uint64_t layout_hash;
    uint64_t key_hash;
    uint64_t indexes_hash;
    uint64_t policy_hash;
    uint64_t wrapper_hash;
    size_t input_column_count;
} VpsQueryFingerprint;

VpsQueryMetadataResult vps_query_fingerprint_build(
    const VpsQueryFingerprintInput *input,
    VpsLogger *logger,
    VpsQueryFingerprint *fingerprint);
VpsQueryChangeClass vps_query_fingerprint_compare(
    const VpsQueryFingerprint *previous,
    const VpsQueryFingerprint *current);
const char *vps_query_change_class_name(VpsQueryChangeClass change_class);

#endif
