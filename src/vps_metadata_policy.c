#include "vps_metadata.h"

#include <string.h>

static int vps_policy_uint32(const unsigned char *value,
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

static int vps_policy_bool(const unsigned char *value,
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

static int vps_policy_attributes(const unsigned char *value,
                                 size_t length,
                                 size_t expected,
                                 int32_t *attributes)
{
    size_t offset = 0U;
    size_t count = 0U;
    while (offset < length) {
        uint32_t parsed = 0U;
        while (offset < length && value[offset] == ' ') ++offset;
        if (offset == length) break;
        if (count >= expected) return 0;
        while (offset < length && value[offset] >= '0' &&
               value[offset] <= '9') {
            uint32_t digit = (uint32_t)(value[offset] - '0');
            if (parsed > (INT32_MAX - digit) / 10U) return 0;
            parsed = parsed * 10U + digit;
            ++offset;
        }
        if (parsed == 0U || (offset < length && value[offset] != ' '))
            return 0;
        attributes[count++] = (int32_t)parsed;
    }
    return count == expected;
}

static int vps_policy_key_contains(const VpsKeyMetadata *key,
                                   int32_t attribute)
{
    size_t index;
    for (index = 0U; index < key->column_count; ++index) {
        if (key->attribute_numbers[index] == attribute) return 1;
    }
    return 0;
}

static void vps_policy_log(VpsLogger *logger,
                           const VpsRelationMetadata *relation,
                           const VpsRelationPolicyMetadata *policy)
{
    VpsLogEvent event;
    static const char operation[] = "relation_policy";
    static const char status[] = "evaluated";
    const char *write_policy;
    uint64_t flags;
    if (logger == NULL || relation == NULL || policy == NULL) return;
    write_policy = vps_relation_write_policy_name(policy->write_policy);
    flags = (policy->row_security ? UINT64_C(1) : UINT64_C(0)) |
            (policy->force_row_security ? UINT64_C(2) : UINT64_C(0)) |
            (relation->is_partition ? UINT64_C(4) : UINT64_C(0));
    if (vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 sizeof(status) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_EXPECTED_CLASS,
                                 write_policy, strlen(write_policy)) !=
            VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_RELATION_OID,
                                 relation->relation_oid) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PARTICIPANT_COUNT,
                                 (uint64_t)policy->parent_count) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_FLAGS, flags) !=
            VPS_LOG_OK) return;
    vps_logger_emit(logger, &event);
}

VpsMetadataResult vps_relation_policy_build(
    const VpsRelationMetadata *relation,
    const VpsKeyMetadata *key,
    const VpsMetadataRowSet *policy_rows,
    VpsLogger *logger,
    VpsRelationPolicyMetadata *policy)
{
    size_t row;
    VpsRelationPolicyMetadata candidate;
    if (relation == NULL || !relation->initialized || key == NULL ||
        policy_rows == NULL || !policy_rows->initialized ||
        policy_rows->field_count != 10U || policy == NULL ||
        policy_rows->row_count == 0U)
        return VPS_METADATA_INVALID_ARGUMENT;
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.write_policy = VPS_RELATION_WRITE_SOURCE_READ_ONLY;
    candidate.row_security = relation->row_security;
    candidate.force_row_security = relation->force_row_security;
    candidate.zero_rows_ambiguous = relation->row_security;
    for (row = 0U; row < policy_rows->row_count; ++row) {
        const unsigned char *value[10];
        size_t length[10];
        int is_null[10];
        size_t field;
        uint32_t relation_oid;
        int row_security;
        int force_row_security;
        int is_partition;
        int has_parent;
        for (field = 0U; field < 10U; ++field) {
            if (vps_metadata_rowset_cell(policy_rows, row, field,
                                         &value[field], &length[field],
                                         &is_null[field]) != VPS_METADATA_OK)
                return VPS_METADATA_INVALID_RESULT;
        }
        if (is_null[0] ||
            !vps_policy_uint32(value[0], length[0], &relation_oid) ||
            relation_oid != relation->relation_oid || is_null[1] ||
            length[1] != 1U || is_null[2] ||
            !vps_policy_bool(value[2], length[2], &is_partition) ||
            is_null[3] ||
            !vps_policy_bool(value[3], length[3], &row_security) ||
            is_null[4] ||
            !vps_policy_bool(value[4], length[4], &force_row_security) ||
            is_null[9] ||
            !vps_policy_bool(value[9], length[9], &has_parent) ||
            row_security != relation->row_security ||
            force_row_security != relation->force_row_security ||
            is_partition != relation->is_partition)
            return VPS_METADATA_INVALID_RESULT;
        if (!is_null[5]) {
            uint32_t parent_oid;
            if (candidate.parent_count >= VPS_METADATA_MAX_KEY_COLUMNS ||
                !vps_policy_uint32(value[5], length[5], &parent_oid) ||
                parent_oid == 0U) return VPS_METADATA_LIMIT_EXCEEDED;
            candidate.parent_oids[candidate.parent_count++] = parent_oid;
        } else if (has_parent) {
            return VPS_METADATA_INVALID_RESULT;
        }
        if (!is_null[6]) {
            uint32_t partition_count;
            if (length[6] != 1U || is_null[7] || is_null[8] ||
                !vps_policy_uint32(value[7], length[7], &partition_count) ||
                partition_count > VPS_METADATA_MAX_KEY_COLUMNS ||
                !vps_policy_attributes(value[8], length[8], partition_count,
                                       candidate.partition_attribute_numbers))
                return VPS_METADATA_INVALID_RESULT;
            candidate.partition_strategy = (char)value[6][0];
            candidate.partition_attribute_count = partition_count;
        }
    }
    if (key->read_only || key->source == VPS_KEY_NONE) {
        candidate.write_policy = VPS_RELATION_WRITE_NO_KEY;
    } else if (relation->kind == VPS_RELATION_INHERITANCE_PARENT) {
        candidate.write_policy = VPS_RELATION_WRITE_INHERITANCE_UNSAFE;
    } else if (!relation->writable_candidate) {
        candidate.write_policy = VPS_RELATION_WRITE_SOURCE_READ_ONLY;
    } else if (relation->kind == VPS_RELATION_PARTITIONED_TABLE) {
        size_t index;
        candidate.write_policy = VPS_RELATION_WRITE_ALLOWED;
        if (candidate.partition_attribute_count == 0U)
            candidate.write_policy = VPS_RELATION_WRITE_PARTITION_KEY_UNPROVEN;
        for (index = 0U; index < candidate.partition_attribute_count; ++index) {
            if (!vps_policy_key_contains(
                    key, candidate.partition_attribute_numbers[index]))
                candidate.write_policy =
                    VPS_RELATION_WRITE_PARTITION_KEY_UNPROVEN;
        }
    } else {
        candidate.write_policy = VPS_RELATION_WRITE_ALLOWED;
    }
    *policy = candidate;
    vps_policy_log(logger, relation, policy);
    return VPS_METADATA_OK;
}

const char *vps_relation_write_policy_name(VpsRelationWritePolicy policy)
{
    switch (policy) {
        case VPS_RELATION_WRITE_ALLOWED: return "allowed";
        case VPS_RELATION_WRITE_NO_KEY: return "no_key";
        case VPS_RELATION_WRITE_SOURCE_READ_ONLY: return "source_read_only";
        case VPS_RELATION_WRITE_INHERITANCE_UNSAFE:
            return "inheritance_unsafe";
        case VPS_RELATION_WRITE_PARTITION_KEY_UNPROVEN:
            return "partition_key_unproven";
        default: return "unknown";
    }
}
