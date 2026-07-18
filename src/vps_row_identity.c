#include "vps_row_identity.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static VpsRowIdentityResult vps_identity_append_u32(VpsBuffer *buffer,
                                                    uint32_t value)
{
    unsigned char encoded[4];
    encoded[0] = (unsigned char)(value >> 24);
    encoded[1] = (unsigned char)(value >> 16);
    encoded[2] = (unsigned char)(value >> 8);
    encoded[3] = (unsigned char)value;
    return vps_buffer_append(buffer, encoded, sizeof(encoded)) == VPS_MEMORY_OK
               ? VPS_ROW_IDENTITY_OK
               : VPS_ROW_IDENTITY_OUT_OF_MEMORY;
}

VpsRowIdentityResult vps_row_identity_stable_integer(
    const VpsClientColumnView *column, int64_t *rowid)
{
    char buffer[64];
    char *end = NULL;
    long long value;
    if (column == NULL || rowid == NULL || column->is_null ||
        column->data == NULL || column->length == 0U ||
        column->length >= sizeof(buffer))
        return VPS_ROW_IDENTITY_INVALID_ARGUMENT;
    (void)memcpy(buffer, column->data, column->length);
    buffer[column->length] = '\0';
    value = strtoll(buffer, &end, 10);
    if (end != buffer + column->length) return VPS_ROW_IDENTITY_MALFORMED;
    *rowid = (int64_t)value;
    return VPS_ROW_IDENTITY_OK;
}

VpsRowIdentityResult vps_row_identity_token(
    const VpsAllocator *allocator,
    const VpsClientColumnView *columns,
    size_t column_count,
    VpsBuffer *token)
{
    size_t index;
    unsigned char header[2] = {VPS_ROW_IDENTITY_FORMAT_VERSION, 0U};
    if (allocator == NULL || columns == NULL || token == NULL ||
        column_count == 0U || column_count > 255U)
        return VPS_ROW_IDENTITY_INVALID_ARGUMENT;
    if (vps_buffer_init(token, allocator, VPS_ROW_IDENTITY_MAX_BYTES) !=
            VPS_MEMORY_OK ||
        vps_buffer_append(token, header, sizeof(header)) != VPS_MEMORY_OK)
        return VPS_ROW_IDENTITY_OUT_OF_MEMORY;
    token->data[1] = (unsigned char)column_count;
    for (index = 0U; index < column_count; ++index) {
        unsigned char tag = columns[index].is_null ? 0U : 1U;
        if (columns[index].length > UINT32_MAX ||
            vps_buffer_append(token, &tag, 1U) != VPS_MEMORY_OK ||
            vps_identity_append_u32(token,
                                    (uint32_t)columns[index].length) !=
                VPS_ROW_IDENTITY_OK ||
            (!columns[index].is_null && columns[index].length != 0U &&
             vps_buffer_append(token, columns[index].data,
                               columns[index].length) != VPS_MEMORY_OK)) {
            vps_buffer_reset(token);
            return columns[index].length > UINT32_MAX
                       ? VPS_ROW_IDENTITY_LIMIT
                       : VPS_ROW_IDENTITY_OUT_OF_MEMORY;
        }
    }
    return VPS_ROW_IDENTITY_OK;
}

VpsRowIdentityResult vps_row_identity_scan_next(uint64_t *counter,
                                                int64_t *rowid)
{
    if (counter == NULL || rowid == NULL || *counter >= (uint64_t)INT64_MAX)
        return VPS_ROW_IDENTITY_LIMIT;
    *counter += 1U;
    *rowid = (int64_t)*counter;
    return VPS_ROW_IDENTITY_OK;
}

const char *vps_row_identity_mode_name(VpsRowIdentityMode mode)
{
    switch (mode) {
        case VPS_ROW_IDENTITY_STABLE_INTEGER: return "stable_integer";
        case VPS_ROW_IDENTITY_HIDDEN_TOKEN: return "hidden_token";
        case VPS_ROW_IDENTITY_SCAN_LOCAL: return "scan_local";
        default: return "unknown";
    }
}
