#include "vps_metadata.h"

#include <string.h>

static int vps_key_uint32(const unsigned char *value,
                          size_t length,
                          uint32_t *parsed)
{
    uint32_t result = 0U;
    size_t index;
    if (value == NULL || parsed == NULL || length == 0U || length > 10U)
        return 0;
    for (index = 0U; index < length; ++index) {
        uint32_t digit;
        if (value[index] < '0' || value[index] > '9') return 0;
        digit = (uint32_t)(value[index] - '0');
        if (result > (UINT32_MAX - digit) / 10U) return 0;
        result = result * 10U + digit;
    }
    *parsed = result;
    return 1;
}

static int vps_key_bool(const unsigned char *value,
                        size_t length,
                        int *parsed)
{
    if (value == NULL || parsed == NULL) return 0;
    if ((length == 1U && value[0] == 't') ||
        (length == 4U && memcmp(value, "true", 4U) == 0)) {
        *parsed = 1;
        return 1;
    }
    if ((length == 1U && value[0] == 'f') ||
        (length == 5U && memcmp(value, "false", 5U) == 0)) {
        *parsed = 0;
        return 1;
    }
    return 0;
}

static int vps_key_cell(const VpsMetadataRowSet *rows,
                        size_t row,
                        size_t field,
                        const unsigned char **value,
                        size_t *length,
                        int *present)
{
    int is_null;
    if (vps_metadata_rowset_cell(rows, row, field, value, length, &is_null) !=
        VPS_METADATA_OK) return 0;
    *present = !is_null;
    return is_null || *value != NULL;
}

static const VpsColumnMetadata *vps_key_column(const VpsColumnSet *columns,
                                               int32_t attribute_number)
{
    size_t index;
    for (index = 0U; index < columns->column_count; ++index) {
        if (columns->columns[index].attribute_number == attribute_number)
            return &columns->columns[index];
    }
    return NULL;
}

static int vps_key_attribute_list(const unsigned char *value,
                                  size_t length,
                                  size_t expected_count,
                                  int32_t *attributes)
{
    size_t offset = 0U;
    size_t count = 0U;
    while (offset < length) {
        uint32_t parsed = 0U;
        while (offset < length && value[offset] == ' ') ++offset;
        if (offset == length) break;
        while (offset < length && value[offset] >= '0' &&
               value[offset] <= '9') {
            uint32_t digit = (uint32_t)(value[offset] - '0');
            if (parsed > (INT32_MAX - digit) / 10U) return 0;
            parsed = parsed * 10U + digit;
            ++offset;
        }
        if (parsed == 0U || (offset < length && value[offset] != ' '))
            return 0;
        if (count < expected_count) attributes[count] = (int32_t)parsed;
        ++count;
    }
    return count >= expected_count;
}

static void vps_key_log(VpsLogger *logger,
                        const VpsKeyMetadata *key,
                        const char *status)
{
    VpsLogEvent event;
    static const char operation[] = "key_discovery";
    const char *source;
    if (logger == NULL || key == NULL || status == NULL) return;
    source = vps_key_source_name(key->source);
    if (vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_EXPECTED_CLASS, source,
                                 strlen(source)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_INDEX_OID,
                                 key->index_oid) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_KEY_COUNT,
                                 (uint64_t)key->column_count) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_FLAGS,
                                 key->nulls_not_distinct ? 1U : 0U) != VPS_LOG_OK)
        return;
    vps_logger_emit(logger, &event);
}

VpsMetadataResult vps_key_discover(
    const VpsMetadataRowSet *index_rows,
    const VpsColumnSet *columns,
    const int32_t *explicit_attribute_numbers,
    size_t explicit_count,
    int explicit_validated,
    VpsLogger *logger,
    VpsKeyMetadata *key)
{
    size_t row;
    VpsKeyMetadata best;
    int best_rank = 0;
    if (index_rows == NULL || !index_rows->initialized ||
        index_rows->field_count != 16U || columns == NULL ||
        !columns->initialized || key == NULL ||
        explicit_count > VPS_METADATA_MAX_KEY_COLUMNS ||
        (explicit_count != 0U && explicit_attribute_numbers == NULL))
        return VPS_METADATA_INVALID_ARGUMENT;
    (void)memset(&best, 0, sizeof(best));
    best.read_only = 1;
    for (row = 0U; row < index_rows->row_count; ++row) {
        const unsigned char *value[16];
        size_t length[16];
        int present[16];
        uint32_t index_oid;
        uint32_t key_count;
        uint32_t total_count;
        int primary;
        int unique;
        int valid;
        int ready;
        int immediate;
        int partial;
        int expression;
        int nulls_not_distinct;
        int deferrable = 0;
        int32_t attributes[VPS_METADATA_MAX_KEY_COLUMNS];
        size_t field;
        size_t column_index;
        int all_not_null = 1;
        int rank;
        for (field = 0U; field < 16U; ++field) {
            if (!vps_key_cell(index_rows, row, field, &value[field],
                              &length[field], &present[field]))
                return VPS_METADATA_INVALID_RESULT;
        }
        if (!present[0] || !vps_key_uint32(value[0], length[0], &index_oid) ||
            !present[1] || !vps_key_bool(value[1], length[1], &primary) ||
            !present[2] || !vps_key_bool(value[2], length[2], &unique) ||
            !present[3] || !vps_key_bool(value[3], length[3], &valid) ||
            !present[4] || !vps_key_bool(value[4], length[4], &ready) ||
            !present[5] || !vps_key_bool(value[5], length[5], &immediate) ||
            !present[6] || !vps_key_bool(value[6], length[6], &partial) ||
            !present[7] || !vps_key_bool(value[7], length[7], &expression) ||
            !present[8] ||
                !vps_key_bool(value[8], length[8], &nulls_not_distinct) ||
            !present[9] || !vps_key_uint32(value[9], length[9], &key_count) ||
            !present[10] ||
                !vps_key_uint32(value[10], length[10], &total_count) ||
            !present[11] || key_count == 0U ||
            key_count > VPS_METADATA_MAX_KEY_COLUMNS || total_count < key_count ||
            (present[12] &&
             !vps_key_bool(value[12], length[12], &deferrable)))
            return VPS_METADATA_INVALID_RESULT;
        if (!unique || !valid || !ready || !immediate || partial || expression ||
            deferrable) continue;
        (void)memset(attributes, 0, sizeof(attributes));
        if (!vps_key_attribute_list(value[11], length[11], key_count,
                                    attributes)) continue;
        for (column_index = 0U; column_index < key_count; ++column_index) {
            const VpsColumnMetadata *column =
                vps_key_column(columns, attributes[column_index]);
            if (column == NULL || column->dropped) {
                all_not_null = 0;
                break;
            }
            if (!column->not_null && !column->domain_not_null)
                all_not_null = 0;
        }
        if (!all_not_null && !nulls_not_distinct) continue;
        rank = primary ? 2 : 1;
        if (rank < best_rank ||
            (rank == best_rank && best.index_oid < index_oid)) continue;
        (void)memset(&best, 0, sizeof(best));
        best.source = primary ? VPS_KEY_PRIMARY : VPS_KEY_UNIQUE;
        best.index_oid = index_oid;
        best.column_count = key_count;
        best.nulls_not_distinct = nulls_not_distinct;
        best.read_only = 0;
        (void)memcpy(best.attribute_numbers, attributes,
                     key_count * sizeof(attributes[0]));
        best_rank = rank;
    }
    if (best_rank == 0 && explicit_count != 0U && explicit_validated) {
        size_t index;
        (void)memset(&best, 0, sizeof(best));
        best.source = VPS_KEY_EXPLICIT;
        best.column_count = explicit_count;
        for (index = 0U; index < explicit_count; ++index) {
            const VpsColumnMetadata *column =
                vps_key_column(columns, explicit_attribute_numbers[index]);
            if (column == NULL || column->dropped ||
                explicit_attribute_numbers[index] <= 0)
                return VPS_METADATA_INVALID_ARGUMENT;
            best.attribute_numbers[index] = explicit_attribute_numbers[index];
        }
    }
    *key = best;
    vps_key_log(logger, key, key->read_only ? "read_only" : "passed");
    return VPS_METADATA_OK;
}

const char *vps_key_source_name(VpsKeySource source)
{
    switch (source) {
        case VPS_KEY_NONE: return "none";
        case VPS_KEY_PRIMARY: return "primary";
        case VPS_KEY_UNIQUE: return "unique";
        case VPS_KEY_EXPLICIT: return "explicit";
        default: return "unknown";
    }
}
