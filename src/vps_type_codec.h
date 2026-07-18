#ifndef VPS_TYPE_CODEC_H
#define VPS_TYPE_CODEC_H

#include "vps_client.h"
#include "vps_memory.h"
#include "vps_type_registry.h"

#include <stdint.h>

#define VPS_TYPE_CODEC_MAX_COLUMN_BYTES (16U * 1024U * 1024U)

typedef enum VpsDecodedKind {
    VPS_DECODED_NULL = 0,
    VPS_DECODED_INTEGER = 1,
    VPS_DECODED_REAL = 2,
    VPS_DECODED_TEXT = 3,
    VPS_DECODED_BLOB = 4
} VpsDecodedKind;

typedef enum VpsTypeCodecResult {
    VPS_TYPE_CODEC_OK = 0,
    VPS_TYPE_CODEC_INVALID_ARGUMENT = 1,
    VPS_TYPE_CODEC_MALFORMED = 2,
    VPS_TYPE_CODEC_RANGE = 3,
    VPS_TYPE_CODEC_LIMIT = 4,
    VPS_TYPE_CODEC_OUT_OF_MEMORY = 5
} VpsTypeCodecResult;

typedef struct VpsDecodedValue {
    VpsDecodedKind kind;
    const void *bytes;
    size_t length;
    int64_t integer;
    double real;
    VpsOwnedMemory owned;
    int initialized;
} VpsDecodedValue;

VpsCodecId vps_type_codec_for_oid(uint32_t type_oid);
VpsTypeCodecResult vps_type_codec_decode(
    const VpsAllocator *allocator,
    VpsCodecId codec,
    const VpsClientColumnView *column,
    VpsDecodedValue *decoded);
void vps_decoded_value_reset(VpsDecodedValue *decoded);
const char *vps_type_codec_result_name(VpsTypeCodecResult result);

#endif
