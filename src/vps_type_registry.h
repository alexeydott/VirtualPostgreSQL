#ifndef VPS_TYPE_REGISTRY_H
#define VPS_TYPE_REGISTRY_H

#include "vps_metadata.h"

#include <stdint.h>

#define VPS_TYPE_REGISTRY_VERSION UINT32_C(1)

#define VPS_CODEC_CAP_READ           (UINT32_C(1) << 0)
#define VPS_CODEC_CAP_EQUALITY       (UINT32_C(1) << 1)
#define VPS_CODEC_CAP_ORDER          (UINT32_C(1) << 2)
#define VPS_CODEC_CAP_DML            (UINT32_C(1) << 3)
#define VPS_CODEC_CAP_PUSHDOWN_EXACT (UINT32_C(1) << 4)
#define VPS_CODEC_CAP_BINARY         (UINT32_C(1) << 5)

typedef enum VpsCodecId {
    VPS_CODEC_BOOLEAN = 0,
    VPS_CODEC_INTEGER = 1,
    VPS_CODEC_FLOAT = 2,
    VPS_CODEC_NUMERIC_TEXT = 3,
    VPS_CODEC_MONEY_TEXT = 4,
    VPS_CODEC_TEXT = 5,
    VPS_CODEC_BYTEA = 6,
    VPS_CODEC_DATETIME_TEXT = 7,
    VPS_CODEC_UUID_TEXT = 8,
    VPS_CODEC_JSON_TEXT = 9,
    VPS_CODEC_ARRAY_TEXT = 10,
    VPS_CODEC_ENUM_TEXT = 11,
    VPS_CODEC_RANGE_TEXT = 12,
    VPS_CODEC_COMPOSITE_TEXT = 13,
    VPS_CODEC_USER_TEXT = 14
} VpsCodecId;

typedef struct VpsTypeSelection {
    uint32_t declared_type_oid;
    uint32_t effective_type_oid;
    VpsCodecId codec;
    uint32_t capabilities;
    int domain;
} VpsTypeSelection;

typedef struct VpsTypeRegistry {
    uint32_t version;
    VpsLogger *logger;
    int initialized;
} VpsTypeRegistry;

VpsMetadataResult vps_type_registry_init(VpsTypeRegistry *registry,
                                         VpsLogger *logger);
VpsMetadataResult vps_type_registry_select(
    const VpsTypeRegistry *registry,
    const VpsColumnSet *columns,
    size_t column_index,
    VpsTypeSelection *selection);
const char *vps_codec_name(VpsCodecId codec);

#endif
