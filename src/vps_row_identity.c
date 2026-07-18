#include "vps_row_identity.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define VPS_IDENTITY_HEADER_BYTES 16U
#define VPS_IDENTITY_FIELD_HEADER_BYTES 9U
#define VPS_IDENTITY_FLAG_OPTIMISTIC 1U

static const unsigned char vps_identity_magic[4] = {'V', 'P', 'S', 'I'};

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

static VpsRowIdentityResult vps_identity_append_u16(VpsBuffer *buffer,
                                                    uint16_t value)
{
    unsigned char encoded[2];
    encoded[0] = (unsigned char)(value >> 8);
    encoded[1] = (unsigned char)value;
    return vps_buffer_append(buffer, encoded, sizeof(encoded)) == VPS_MEMORY_OK
               ? VPS_ROW_IDENTITY_OK
               : VPS_ROW_IDENTITY_OUT_OF_MEMORY;
}

static VpsRowIdentityResult vps_identity_append_u64(VpsBuffer *buffer,
                                                    uint64_t value)
{
    unsigned char encoded[8];
    size_t index;
    for (index = 0U; index < sizeof(encoded); ++index)
        encoded[index] = (unsigned char)(value >> (56U - index * 8U));
    return vps_buffer_append(buffer, encoded, sizeof(encoded)) == VPS_MEMORY_OK
               ? VPS_ROW_IDENTITY_OK
               : VPS_ROW_IDENTITY_OUT_OF_MEMORY;
}

static uint16_t vps_identity_read_u16(const unsigned char *value)
{
    return (uint16_t)(((uint16_t)value[0] << 8) | value[1]);
}

static uint32_t vps_identity_read_u32(const unsigned char *value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | (uint32_t)value[3];
}

static uint64_t vps_identity_read_u64(const unsigned char *value)
{
    uint64_t result = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index)
        result = (result << 8) | value[index];
    return result;
}

static VpsRowIdentityResult vps_identity_append_field(
    VpsBuffer *token,
    const VpsRowIdentityField *field)
{
    unsigned char kind;
    uint32_t length;
    if (field == NULL || field->kind < VPS_ROW_IDENTITY_FIELD_NULL ||
        field->kind > VPS_ROW_IDENTITY_FIELD_BLOB ||
        field->length > UINT32_MAX ||
        ((field->kind == VPS_ROW_IDENTITY_FIELD_TEXT ||
          field->kind == VPS_ROW_IDENTITY_FIELD_BLOB) &&
         field->length != 0U && field->bytes == NULL) ||
        (field->kind == VPS_ROW_IDENTITY_FIELD_NULL && field->length != 0U))
        return field != NULL && field->length > UINT32_MAX
                   ? VPS_ROW_IDENTITY_LIMIT
                   : VPS_ROW_IDENTITY_INVALID_ARGUMENT;
    kind = (unsigned char)field->kind;
    length = field->kind == VPS_ROW_IDENTITY_FIELD_INTEGER ? 8U
                                                            : (uint32_t)field->length;
    if (vps_buffer_append(token, &kind, 1U) != VPS_MEMORY_OK ||
        vps_identity_append_u32(token, field->type_oid) != VPS_ROW_IDENTITY_OK ||
        vps_identity_append_u32(token, length) != VPS_ROW_IDENTITY_OK)
        return VPS_ROW_IDENTITY_OUT_OF_MEMORY;
    if (field->kind == VPS_ROW_IDENTITY_FIELD_INTEGER)
        return vps_identity_append_u64(token, (uint64_t)field->integer);
    if (field->kind != VPS_ROW_IDENTITY_FIELD_NULL && length != 0U &&
        vps_buffer_append(token, field->bytes, length) != VPS_MEMORY_OK)
        return VPS_ROW_IDENTITY_OUT_OF_MEMORY;
    return VPS_ROW_IDENTITY_OK;
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
    VpsRowIdentityField fields[VPS_ROW_IDENTITY_MAX_FIELDS];
    VpsRowIdentitySpec spec;
    size_t index;
    if (allocator == NULL || columns == NULL || token == NULL ||
        column_count == 0U || column_count > VPS_ROW_IDENTITY_MAX_FIELDS)
        return VPS_ROW_IDENTITY_INVALID_ARGUMENT;
    (void)memset(fields, 0, sizeof(fields));
    for (index = 0U; index < column_count; ++index) {
        fields[index].kind = columns[index].is_null
                                 ? VPS_ROW_IDENTITY_FIELD_NULL
                                 : VPS_ROW_IDENTITY_FIELD_TEXT;
        fields[index].type_oid = columns[index].type_oid;
        fields[index].bytes = columns[index].data;
        fields[index].length = columns[index].length;
    }
    spec.relation_oid = 0U;
    spec.key_fields = fields;
    spec.key_field_count = column_count;
    spec.optimistic_field = NULL;
    return vps_row_identity_encode(allocator, &spec, token);
}

VpsRowIdentityResult vps_row_identity_encode(
    const VpsAllocator *allocator,
    const VpsRowIdentitySpec *spec,
    VpsBuffer *token)
{
    unsigned char prefix[6];
    size_t index;
    VpsRowIdentityResult result;
    if (allocator == NULL || !vps_allocator_is_valid(allocator) ||
        spec == NULL || token == NULL ||
        spec->key_fields == NULL || spec->key_field_count == 0U ||
        spec->key_field_count > VPS_ROW_IDENTITY_MAX_FIELDS)
        return VPS_ROW_IDENTITY_INVALID_ARGUMENT;
    (void)memcpy(prefix, vps_identity_magic, sizeof(vps_identity_magic));
    prefix[4] = VPS_ROW_IDENTITY_FORMAT_VERSION;
    prefix[5] = spec->optimistic_field != NULL
                    ? VPS_IDENTITY_FLAG_OPTIMISTIC : 0U;
    if (vps_buffer_init(token, allocator, VPS_ROW_IDENTITY_MAX_BYTES) !=
            VPS_MEMORY_OK ||
        vps_buffer_append(token, prefix, sizeof(prefix)) != VPS_MEMORY_OK ||
        vps_identity_append_u16(token, (uint16_t)spec->key_field_count) !=
            VPS_ROW_IDENTITY_OK ||
        vps_identity_append_u32(token, spec->relation_oid) !=
            VPS_ROW_IDENTITY_OK ||
        vps_identity_append_u32(token, 0U) != VPS_ROW_IDENTITY_OK) {
        vps_buffer_reset(token);
        return VPS_ROW_IDENTITY_OUT_OF_MEMORY;
    }
    for (index = 0U; index < spec->key_field_count; ++index) {
        result = vps_identity_append_field(token, &spec->key_fields[index]);
        if (result != VPS_ROW_IDENTITY_OK) {
            vps_buffer_reset(token);
            return result;
        }
    }
    if (spec->optimistic_field != NULL) {
        result = vps_identity_append_field(token, spec->optimistic_field);
        if (result != VPS_ROW_IDENTITY_OK) {
            vps_buffer_reset(token);
            return result;
        }
    }
    token->data[12] = (unsigned char)(token->size >> 24);
    token->data[13] = (unsigned char)(token->size >> 16);
    token->data[14] = (unsigned char)(token->size >> 8);
    token->data[15] = (unsigned char)token->size;
    return VPS_ROW_IDENTITY_OK;
}

static VpsRowIdentityResult vps_identity_decode_field(
    const unsigned char *token,
    size_t token_length,
    size_t *offset,
    VpsRowIdentityField *field)
{
    uint32_t length;
    uint64_t encoded_integer;
    if (*offset > token_length ||
        token_length - *offset < VPS_IDENTITY_FIELD_HEADER_BYTES)
        return VPS_ROW_IDENTITY_MALFORMED;
    (void)memset(field, 0, sizeof(*field));
    field->kind = (VpsRowIdentityFieldKind)token[*offset];
    field->type_oid = vps_identity_read_u32(token + *offset + 1U);
    length = vps_identity_read_u32(token + *offset + 5U);
    *offset += VPS_IDENTITY_FIELD_HEADER_BYTES;
    if (field->kind < VPS_ROW_IDENTITY_FIELD_NULL ||
        field->kind > VPS_ROW_IDENTITY_FIELD_BLOB ||
        (field->kind == VPS_ROW_IDENTITY_FIELD_NULL && length != 0U) ||
        (field->kind == VPS_ROW_IDENTITY_FIELD_INTEGER && length != 8U) ||
        length > token_length - *offset)
        return VPS_ROW_IDENTITY_MALFORMED;
    if (field->kind == VPS_ROW_IDENTITY_FIELD_INTEGER) {
        encoded_integer = vps_identity_read_u64(token + *offset);
        field->integer = (int64_t)encoded_integer;
    } else if (field->kind != VPS_ROW_IDENTITY_FIELD_NULL) {
        field->bytes = token + *offset;
        field->length = length;
    }
    *offset += length;
    return VPS_ROW_IDENTITY_OK;
}

VpsRowIdentityResult vps_row_identity_decode(
    const void *token_value,
    size_t token_length,
    VpsRowIdentityView *view)
{
    const unsigned char *token = (const unsigned char *)token_value;
    VpsRowIdentityView candidate;
    uint16_t field_count;
    unsigned char flags;
    size_t offset = VPS_IDENTITY_HEADER_BYTES;
    size_t index;
    VpsRowIdentityResult result;
    if (token == NULL || view == NULL)
        return VPS_ROW_IDENTITY_INVALID_ARGUMENT;
    if (token_length < VPS_IDENTITY_HEADER_BYTES ||
        token_length > VPS_ROW_IDENTITY_MAX_BYTES ||
        memcmp(token, vps_identity_magic, sizeof(vps_identity_magic)) != 0 ||
        token[4] != VPS_ROW_IDENTITY_FORMAT_VERSION ||
        (token[5] & UINT8_C(0xfe)) != 0U ||
        vps_identity_read_u32(token + 12U) != token_length)
        return VPS_ROW_IDENTITY_MALFORMED;
    flags = token[5];
    field_count = vps_identity_read_u16(token + 6U);
    if (field_count == 0U || field_count > VPS_ROW_IDENTITY_MAX_FIELDS)
        return VPS_ROW_IDENTITY_MALFORMED;
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.relation_oid = vps_identity_read_u32(token + 8U);
    candidate.key_field_count = field_count;
    for (index = 0U; index < field_count; ++index) {
        result = vps_identity_decode_field(token, token_length, &offset,
                                           &candidate.key_fields[index]);
        if (result != VPS_ROW_IDENTITY_OK) return result;
    }
    if ((flags & VPS_IDENTITY_FLAG_OPTIMISTIC) != 0U) {
        result = vps_identity_decode_field(token, token_length, &offset,
                                           &candidate.optimistic_field);
        if (result != VPS_ROW_IDENTITY_OK) return result;
        candidate.has_optimistic_field = 1;
    }
    if (offset != token_length) return VPS_ROW_IDENTITY_MALFORMED;
    *view = candidate;
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
