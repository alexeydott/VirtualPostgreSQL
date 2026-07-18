#include "vps_type_codec.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int vps_hex_digit(unsigned char value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
    if (value >= 'A' && value <= 'F') return (int)(value - 'A') + 10;
    return -1;
}

static int vps_uuid_valid(const unsigned char *value, size_t length)
{
    size_t index;
    if (length != 36U) return 0;
    for (index = 0U; index < length; ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') return 0;
        } else if (vps_hex_digit(value[index]) < 0 ||
                   (value[index] >= 'A' && value[index] <= 'F')) {
            return 0;
        }
    }
    return 1;
}

static VpsTypeCodecResult vps_parse_integer(const VpsClientColumnView *column,
                                            int64_t *value)
{
    char buffer[64];
    char *end = NULL;
    long long parsed;
    if (column->length == 0U || column->length >= sizeof(buffer))
        return VPS_TYPE_CODEC_MALFORMED;
    (void)memcpy(buffer, column->data, column->length);
    buffer[column->length] = '\0';
    errno = 0;
    parsed = strtoll(buffer, &end, 10);
    if (errno == ERANGE) return VPS_TYPE_CODEC_RANGE;
    if (end != buffer + column->length) return VPS_TYPE_CODEC_MALFORMED;
    *value = (int64_t)parsed;
    return VPS_TYPE_CODEC_OK;
}

static int vps_float_special(const char *value, size_t length)
{
    return (length == 3U && memcmp(value, "NaN", 3U) == 0) ||
           (length == 8U && memcmp(value, "Infinity", 8U) == 0) ||
           (length == 9U && memcmp(value, "-Infinity", 9U) == 0);
}

static VpsTypeCodecResult vps_parse_real(const VpsClientColumnView *column,
                                         double *value)
{
    char buffer[128];
    char *end = NULL;
    double parsed;
    if (column->length == 0U || column->length >= sizeof(buffer))
        return VPS_TYPE_CODEC_MALFORMED;
    (void)memcpy(buffer, column->data, column->length);
    buffer[column->length] = '\0';
    errno = 0;
    parsed = strtod(buffer, &end);
    if (errno == ERANGE) return VPS_TYPE_CODEC_RANGE;
    if (end != buffer + column->length) return VPS_TYPE_CODEC_MALFORMED;
    *value = parsed;
    return VPS_TYPE_CODEC_OK;
}

VpsCodecId vps_type_codec_for_oid(uint32_t type_oid)
{
    return vps_type_registry_builtin_oid_codec(type_oid);
}

VpsTypeCodecResult vps_type_codec_decode(
    const VpsAllocator *allocator,
    VpsCodecId codec,
    const VpsClientColumnView *column,
    VpsDecodedValue *decoded)
{
    VpsTypeCodecResult result = VPS_TYPE_CODEC_OK;
    if (allocator == NULL || column == NULL || decoded == NULL ||
        !vps_allocator_is_valid(allocator) ||
        (!column->is_null && column->length != 0U && column->data == NULL) ||
        column->length > VPS_TYPE_CODEC_MAX_COLUMN_BYTES)
        return column != NULL && column->length > VPS_TYPE_CODEC_MAX_COLUMN_BYTES
                   ? VPS_TYPE_CODEC_LIMIT
                   : VPS_TYPE_CODEC_INVALID_ARGUMENT;
    (void)memset(decoded, 0, sizeof(*decoded));
    if (vps_owned_memory_init(&decoded->owned, allocator) != VPS_MEMORY_OK)
        return VPS_TYPE_CODEC_INVALID_ARGUMENT;
    decoded->initialized = 1;
    if (column->is_null) {
        decoded->kind = VPS_DECODED_NULL;
        return VPS_TYPE_CODEC_OK;
    }
    decoded->bytes = column->data;
    decoded->length = column->length;
    switch (codec) {
        case VPS_CODEC_BOOLEAN:
            if (column->length != 1U ||
                (((const char *)column->data)[0] != 't' &&
                 ((const char *)column->data)[0] != 'f'))
                result = VPS_TYPE_CODEC_MALFORMED;
            else {
                decoded->kind = VPS_DECODED_INTEGER;
                decoded->integer = ((const char *)column->data)[0] == 't';
            }
            break;
        case VPS_CODEC_INTEGER:
            decoded->kind = VPS_DECODED_INTEGER;
            result = vps_parse_integer(column, &decoded->integer);
            break;
        case VPS_CODEC_FLOAT:
            if (vps_float_special((const char *)column->data,
                                  column->length)) {
                decoded->kind = VPS_DECODED_TEXT;
            } else {
                decoded->kind = VPS_DECODED_REAL;
                result = vps_parse_real(column, &decoded->real);
            }
            break;
        case VPS_CODEC_BYTEA: {
            const unsigned char *source = (const unsigned char *)column->data;
            size_t source_index;
            size_t bytes;
            unsigned char *target;
            if (column->length < 2U || source[0] != '\\' || source[1] != 'x' ||
                ((column->length - 2U) & 1U) != 0U) {
                result = VPS_TYPE_CODEC_MALFORMED;
                break;
            }
            bytes = (column->length - 2U) / 2U;
            if (bytes != 0U &&
                vps_owned_memory_allocate(&decoded->owned, bytes) !=
                    VPS_MEMORY_OK) {
                result = VPS_TYPE_CODEC_OUT_OF_MEMORY;
                break;
            }
            target = (unsigned char *)decoded->owned.memory;
            for (source_index = 2U; source_index < column->length;
                 source_index += 2U) {
                int high = vps_hex_digit(source[source_index]);
                int low = vps_hex_digit(source[source_index + 1U]);
                if (high < 0 || low < 0) {
                    result = VPS_TYPE_CODEC_MALFORMED;
                    break;
                }
                target[(source_index - 2U) / 2U] =
                    (unsigned char)((high << 4) | low);
            }
            decoded->kind = VPS_DECODED_BLOB;
            decoded->bytes = decoded->owned.memory;
            decoded->length = bytes;
            break;
        }
        case VPS_CODEC_UUID_TEXT:
            if (!vps_uuid_valid((const unsigned char *)column->data,
                                column->length))
                result = VPS_TYPE_CODEC_MALFORMED;
            decoded->kind = VPS_DECODED_TEXT;
            break;
        default:
            decoded->kind = VPS_DECODED_TEXT;
            break;
    }
    if (result != VPS_TYPE_CODEC_OK) vps_decoded_value_reset(decoded);
    return result;
}

void vps_decoded_value_reset(VpsDecodedValue *decoded)
{
    if (decoded == NULL) return;
    if (decoded->initialized) vps_owned_memory_release(&decoded->owned);
    (void)memset(decoded, 0, sizeof(*decoded));
}

const char *vps_type_codec_result_name(VpsTypeCodecResult result)
{
    switch (result) {
        case VPS_TYPE_CODEC_OK: return "ok";
        case VPS_TYPE_CODEC_INVALID_ARGUMENT: return "invalid_argument";
        case VPS_TYPE_CODEC_MALFORMED: return "malformed";
        case VPS_TYPE_CODEC_RANGE: return "range";
        case VPS_TYPE_CODEC_LIMIT: return "limit";
        case VPS_TYPE_CODEC_OUT_OF_MEMORY: return "out_of_memory";
        default: return "unknown";
    }
}
