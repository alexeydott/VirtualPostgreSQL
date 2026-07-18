#include "vps_query_indexes.h"

#include <ctype.h>
#include <string.h>

#define VPS_QUERY_INDEX_HASH_OFFSET UINT64_C(1469598103934665603)
#define VPS_QUERY_INDEX_HASH_PRIME UINT64_C(1099511628211)

static int vps_query_name_equal(const char *left, size_t left_length,
                                const char *right, size_t right_length)
{
    size_t index;
    if (left_length != right_length) return 0;
    for (index = 0U; index < left_length; ++index) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a < 0x80U) a = (unsigned char)tolower(a);
        if (b < 0x80U) b = (unsigned char)tolower(b);
        if (a != b) return 0;
    }
    return 1;
}

static uint64_t vps_query_name_hash(const char *name, size_t length)
{
    uint64_t hash = VPS_QUERY_INDEX_HASH_OFFSET;
    size_t index;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)name[index];
        if (value < 0x80U) value = (unsigned char)tolower(value);
        hash = (hash ^ (uint64_t)value) * VPS_QUERY_INDEX_HASH_PRIME;
    }
    return hash;
}

static int vps_query_identifier(const char *text, size_t length)
{
    size_t index;
    if (length == 0U ||
        !(text[0] == '_' || isalpha((unsigned char)text[0]) != 0 ||
          (unsigned char)text[0] >= 0x80U)) return 0;
    for (index = 1U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!(value == '_' || value == '$' || isalnum(value) != 0 ||
              value >= 0x80U)) return 0;
    }
    return 1;
}

static void vps_query_trim(const char *text, size_t *start, size_t *end)
{
    while (*start < *end &&
           isspace((unsigned char)text[*start]) != 0) ++*start;
    while (*end > *start &&
           isspace((unsigned char)text[*end - 1U]) != 0) --*end;
}

static int vps_query_find_column(const VpsQueryDescribeResult *described,
                                 const char *name, size_t length,
                                 uint16_t *column)
{
    size_t index;
    for (index = 0U; index < described->field_count; ++index) {
        if (vps_query_name_equal(name, length,
                                 described->fields[index].name,
                                 described->fields[index].name_length)) {
            *column = (uint16_t)index;
            return 1;
        }
    }
    return 0;
}

VpsQueryIndexesResult vps_query_indexes_parse(
    const char *definition,
    size_t definition_length,
    const VpsQueryDescribeResult *described,
    VpsQueryIndexSet *indexes)
{
    size_t position = 0U;
    if (described == NULL || !described->initialized || indexes == NULL ||
        (definition_length != 0U && definition == NULL)) {
        return VPS_QUERY_INDEXES_INVALID_ARGUMENT;
    }
    (void)memset(indexes, 0, sizeof(*indexes));
    indexes->format_version = VPS_QUERY_INDEXES_FORMAT_VERSION;
    if (definition_length == 0U) return VPS_QUERY_INDEXES_OK;
    if (definition_length > VPS_QUERY_INDEXES_MAX_BYTES) {
        return VPS_QUERY_INDEXES_LIMIT_EXCEEDED;
    }
    while (position < definition_length) {
        VpsQueryIndexDefinition *index_definition;
        size_t item_end = position;
        size_t equals = SIZE_MAX;
        size_t name_start;
        size_t name_end;
        size_t column_position;
        size_t prior;
        while (item_end < definition_length && definition[item_end] != ';') {
            if (definition[item_end] == '=' && equals == SIZE_MAX) {
                equals = item_end;
            }
            ++item_end;
        }
        if (indexes->index_count >= VPS_QUERY_INDEX_MAX_COUNT) {
            return VPS_QUERY_INDEXES_LIMIT_EXCEEDED;
        }
        if (equals == SIZE_MAX || equals == position || equals + 1U >= item_end) {
            return VPS_QUERY_INDEXES_INVALID_SYNTAX;
        }
        name_start = position;
        name_end = equals;
        vps_query_trim(definition, &name_start, &name_end);
        if (name_end - name_start > VPS_CLIENT_MAX_FIELD_NAME_BYTES ||
            !vps_query_identifier(definition + name_start,
                                  name_end - name_start)) {
            return VPS_QUERY_INDEXES_INVALID_SYNTAX;
        }
        for (prior = 0U; prior < indexes->index_count; ++prior) {
            if (vps_query_name_equal(
                    definition + name_start, name_end - name_start,
                    indexes->indexes[prior].name,
                    indexes->indexes[prior].name_length)) {
                return VPS_QUERY_INDEXES_DUPLICATE_NAME;
            }
        }
        index_definition = &indexes->indexes[indexes->index_count];
        (void)memcpy(index_definition->name, definition + name_start,
                     name_end - name_start);
        index_definition->name_length = name_end - name_start;
        index_definition->name_hash = vps_query_name_hash(
            index_definition->name, index_definition->name_length);
        column_position = equals + 1U;
        while (column_position < item_end) {
            size_t column_end = column_position;
            size_t delimiter;
            size_t column_start;
            uint16_t column;
            size_t existing;
            while (column_end < item_end && definition[column_end] != ',') {
                ++column_end;
            }
            delimiter = column_end;
            column_start = column_position;
            vps_query_trim(definition, &column_start, &column_end);
            if (index_definition->column_count >=
                    VPS_QUERY_INDEX_MAX_COLUMNS ||
                !vps_query_identifier(definition + column_start,
                                      column_end - column_start)) {
                return index_definition->column_count >=
                               VPS_QUERY_INDEX_MAX_COLUMNS
                           ? VPS_QUERY_INDEXES_LIMIT_EXCEEDED
                           : VPS_QUERY_INDEXES_INVALID_SYNTAX;
            }
            if (!vps_query_find_column(described, definition + column_start,
                                       column_end - column_start, &column)) {
                return VPS_QUERY_INDEXES_UNKNOWN_COLUMN;
            }
            for (existing = 0U;
                 existing < index_definition->column_count; ++existing) {
                if (index_definition->columns[existing] == column) {
                    return VPS_QUERY_INDEXES_DUPLICATE_COLUMN;
                }
            }
            index_definition->columns[index_definition->column_count++] =
                column;
            column_position = delimiter + 1U;
        }
        if (index_definition->column_count == 0U) {
            return VPS_QUERY_INDEXES_INVALID_SYNTAX;
        }
        ++indexes->index_count;
        position = item_end + 1U;
        if (item_end < definition_length && position == definition_length) {
            return VPS_QUERY_INDEXES_INVALID_SYNTAX;
        }
    }
    return VPS_QUERY_INDEXES_OK;
}

const char *vps_query_indexes_result_name(VpsQueryIndexesResult result)
{
    static const char *const names[] = {
        "ok", "invalid_argument", "limit_exceeded", "invalid_syntax",
        "duplicate_name", "unknown_column", "duplicate_column"
    };
    if (result < VPS_QUERY_INDEXES_OK ||
        result > VPS_QUERY_INDEXES_DUPLICATE_COLUMN) return "unknown";
    return names[(size_t)result];
}
