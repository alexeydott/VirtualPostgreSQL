#include "vps_dml.h"

#include <stdio.h>
#include <string.h>

static int vps_dml_append(VpsBuffer *buffer,
                          const char *value,
                          size_t length)
{
    return vps_buffer_append(buffer, value, length) == VPS_MEMORY_OK;
}

static int vps_dml_identifier(VpsBuffer *buffer,
                              const unsigned char *name,
                              size_t length)
{
    size_t index;
    if (name == NULL || length == 0U || !vps_dml_append(buffer, "\"", 1U))
        return 0;
    for (index = 0U; index < length; ++index) {
        if (name[index] == '\0') return 0;
        if (name[index] == '"' && !vps_dml_append(buffer, "\"", 1U))
            return 0;
        if (!vps_dml_append(buffer, (const char *)&name[index], 1U)) return 0;
    }
    return vps_dml_append(buffer, "\"", 1U);
}

static int vps_dml_column_name(const VpsDmlPolicy *policy,
                               size_t visible,
                               const unsigned char **name,
                               size_t *length)
{
    size_t metadata_index;
    if (policy == NULL || visible >= policy->visible_count)
        return 0;
    metadata_index = policy->visible_to_metadata[visible];
    return vps_column_set_string(
               &policy->metadata->columns,
               &policy->metadata->columns.columns[metadata_index].name,
               name, length) == VPS_METADATA_OK;
}

static int vps_dml_target(VpsBuffer *query, const VpsDmlPolicy *policy)
{
    const VpsRelationMetadata *relation = &policy->metadata->relation;
    return vps_dml_identifier(query, relation->schema_name.data,
                              relation->schema_name.size) &&
           vps_dml_append(query, ".", 1U) &&
           vps_dml_identifier(query, relation->relation_name.data,
                              relation->relation_name.size);
}

static int vps_dml_placeholder(VpsBuffer *query, size_t number)
{
    char value[32];
    int length = snprintf(value, sizeof(value), "$%llu",
                          (unsigned long long)number);
    return length > 0 && (size_t)length < sizeof(value) &&
           vps_dml_append(query, value, (size_t)length);
}

static int vps_dml_find_visible_by_attribute(const VpsDmlPolicy *policy,
                                             int32_t attribute_number,
                                             size_t *visible)
{
    size_t index;
    for (index = 0U; index < policy->visible_count; ++index) {
        const VpsColumnMetadata *column =
            &policy->metadata->columns.columns[
                policy->visible_to_metadata[index]];
        if (column->attribute_number == attribute_number) {
            *visible = index;
            return 1;
        }
    }
    return 0;
}

VpsDmlResult vps_dml_policy_build(
    const VpsTableMetadata *metadata,
    int mode_rw,
    VpsDmlOptimisticMode optimistic_mode,
    const char *version_column,
    size_t version_column_length,
    VpsDmlPolicy *policy)
{
    size_t metadata_index;
    size_t key_index;
    int version_found = optimistic_mode != VPS_DML_OPTIMISTIC_COLUMN;
    if (metadata == NULL || !metadata->loaded || policy == NULL ||
        optimistic_mode > VPS_DML_OPTIMISTIC_XMIN ||
        (optimistic_mode == VPS_DML_OPTIMISTIC_COLUMN &&
         (version_column == NULL || version_column_length == 0U)))
        return VPS_DML_INVALID_ARGUMENT;
    (void)memset(policy, 0, sizeof(*policy));
    policy->metadata = metadata;
    policy->optimistic_mode = optimistic_mode;
    policy->version_visible = SIZE_MAX;
    if (!mode_rw) return VPS_DML_READ_ONLY;
    if (metadata->relation.kind != VPS_RELATION_TABLE &&
        metadata->relation.kind != VPS_RELATION_PARTITIONED_TABLE &&
        metadata->relation.kind != VPS_RELATION_PARTITION)
        return VPS_DML_UNSUPPORTED_RELATION;
    if (metadata->policy.write_policy != VPS_RELATION_WRITE_ALLOWED)
        return metadata->policy.write_policy == VPS_RELATION_WRITE_NO_KEY
                   ? VPS_DML_NO_KEY : VPS_DML_UNSUPPORTED_RELATION;
    if (metadata->key.source == VPS_KEY_NONE || metadata->key.read_only ||
        metadata->key.column_count == 0U)
        return VPS_DML_NO_KEY;
    for (metadata_index = 0U;
         metadata_index < metadata->columns.column_count; ++metadata_index) {
        const VpsColumnMetadata *column =
            &metadata->columns.columns[metadata_index];
        VpsTypeSelection selection;
        const unsigned char *name = NULL;
        size_t name_length = 0U;
        size_t visible;
        if (column->dropped) continue;
        visible = policy->visible_count;
        if (visible >= VPS_DML_MAX_COLUMNS || metadata_index > UINT16_MAX)
            return VPS_DML_LIMIT;
        if (vps_type_registry_select(&metadata->type_registry,
                                     &metadata->columns, metadata_index,
                                     &selection) != VPS_METADATA_OK)
            return VPS_DML_UNSUPPORTED_TYPE;
        policy->visible_to_metadata[visible] = (uint16_t)metadata_index;
        policy->selections[visible] = selection;
        if ((selection.capabilities & VPS_CODEC_CAP_DML) != 0U &&
            column->generated_kind == '\0') {
            policy->updatable[visible] = column->identity_kind == '\0';
            policy->insertable[visible] = column->identity_kind != 'a';
        }
        if (optimistic_mode == VPS_DML_OPTIMISTIC_COLUMN &&
            vps_column_set_string(&metadata->columns, &column->name,
                                  &name, &name_length) == VPS_METADATA_OK &&
            name_length == version_column_length &&
            memcmp(name, version_column, name_length) == 0) {
            if (!column->not_null || column->generated_kind != '\0' ||
                (selection.capabilities & VPS_CODEC_CAP_DML) == 0U)
                return VPS_DML_OPTIMISTIC_INVALID;
            policy->version_visible = visible;
            version_found = 1;
        }
        policy->visible_count += 1U;
    }
    if (!version_found) return VPS_DML_OPTIMISTIC_INVALID;
    if (metadata->key.column_count > VPS_METADATA_MAX_KEY_COLUMNS)
        return VPS_DML_LIMIT;
    for (key_index = 0U; key_index < metadata->key.column_count; ++key_index) {
        size_t visible;
        if (!vps_dml_find_visible_by_attribute(
                policy, metadata->key.attribute_numbers[key_index],
                &visible) ||
            (policy->selections[visible].capabilities & VPS_CODEC_CAP_DML) == 0U)
            return VPS_DML_UNSUPPORTED_TYPE;
        policy->key_visible[key_index] = (uint16_t)visible;
    }
    policy->key_count = metadata->key.column_count;
    policy->writable = 1;
    return VPS_DML_OK;
}

static VpsDmlResult vps_dml_add_parameter(VpsDmlPlan *plan,
                                          VpsDmlParameterSource source,
                                          size_t index,
                                          uint32_t type_oid)
{
    VpsDmlParameterSlot *slot;
    if (plan->parameter_count >= VPS_DML_MAX_PARAMETERS || index > UINT16_MAX)
        return VPS_DML_LIMIT;
    slot = &plan->parameters[plan->parameter_count++];
    slot->source = source;
    slot->index = (uint16_t)index;
    slot->type_oid = type_oid;
    return VPS_DML_OK;
}

static VpsDmlResult vps_dml_write_value(VpsDmlPlan *plan,
                                        const VpsDmlPolicy *policy,
                                        size_t visible)
{
    uint32_t value_parameter = (uint32_t)plan->parameter_count + 1U;
    if (policy->spatial_kind[visible] != 0U &&
        policy->spatial_format != VPS_SPATIAL_FORMAT_NONE) {
        VpsSpatialExpression expression;
        VpsSpatialKind kind = policy->spatial_kind[visible] == 1U
                                  ? VPS_SPATIAL_KIND_GEOMETRY
                                  : VPS_SPATIAL_KIND_GEOGRAPHY;
        uint32_t srid_parameter =
            policy->spatial_format == VPS_SPATIAL_FORMAT_WKT ||
                    policy->spatial_format == VPS_SPATIAL_FORMAT_WKB
                ? value_parameter + 1U : 0U;
        uint32_t value_oid =
            policy->spatial_format == VPS_SPATIAL_FORMAT_WKB ||
                    policy->spatial_format == VPS_SPATIAL_FORMAT_EWKB
                ? UINT32_C(17) : UINT32_C(25);
        if (policy->spatial == NULL ||
            vps_spatial_write_expression(
                policy->spatial, kind, policy->spatial_format,
                value_parameter, srid_parameter, &expression) !=
                VPS_SPATIAL_OK ||
            !vps_dml_append(&plan->query, expression.sql,
                            expression.length) ||
            vps_dml_add_parameter(plan, VPS_DML_PARAMETER_NEW_VALUE,
                                  visible, value_oid) != VPS_DML_OK)
            return VPS_DML_OUT_OF_MEMORY;
        if (srid_parameter != 0U &&
            vps_dml_add_parameter(plan, VPS_DML_PARAMETER_SPATIAL_SRID,
                                  visible, UINT32_C(23)) != VPS_DML_OK)
            return VPS_DML_OUT_OF_MEMORY;
        return VPS_DML_OK;
    }
    if (!vps_dml_placeholder(&plan->query, value_parameter) ||
        vps_dml_add_parameter(
            plan, VPS_DML_PARAMETER_NEW_VALUE, visible,
            policy->selections[visible].declared_type_oid) != VPS_DML_OK)
        return VPS_DML_OUT_OF_MEMORY;
    return VPS_DML_OK;
}

static VpsDmlResult vps_dml_returning(VpsDmlPlan *plan,
                                      const VpsDmlPolicy *policy)
{
    size_t key_index;
    if (!vps_dml_append(&plan->query, " RETURNING ", 11U))
        return VPS_DML_OUT_OF_MEMORY;
    for (key_index = 0U; key_index < policy->key_count; ++key_index) {
        const unsigned char *name;
        size_t length;
        size_t visible = policy->key_visible[key_index];
        if ((key_index != 0U && !vps_dml_append(&plan->query, ",", 1U)) ||
            !vps_dml_column_name(policy, visible, &name, &length) ||
            !vps_dml_identifier(&plan->query, name, length))
            return VPS_DML_OUT_OF_MEMORY;
        plan->returning_visible[plan->returning_count++] = (uint16_t)visible;
    }
    plan->key_returning_count = policy->key_count;
    if (policy->optimistic_mode == VPS_DML_OPTIMISTIC_COLUMN) {
        const unsigned char *name;
        size_t length;
        if (!vps_dml_column_name(policy, policy->version_visible,
                                 &name, &length) ||
            !vps_dml_append(&plan->query, ",", 1U) ||
            !vps_dml_identifier(&plan->query, name, length))
            return VPS_DML_OUT_OF_MEMORY;
        plan->returning_visible[plan->returning_count++] =
            (uint16_t)policy->version_visible;
        plan->returns_optimistic = 1;
    } else if (policy->optimistic_mode == VPS_DML_OPTIMISTIC_XMIN) {
        if (!vps_dml_append(&plan->query,
                            ",xmin::pg_catalog.text", 22U))
            return VPS_DML_OUT_OF_MEMORY;
        plan->returning_visible[plan->returning_count++] = UINT16_MAX;
        plan->returns_optimistic = 1;
    }
    return VPS_DML_OK;
}

static VpsDmlResult vps_dml_original_predicate(
    VpsDmlPlan *plan,
    const VpsDmlPolicy *policy,
    const VpsRowIdentityView *identity)
{
    size_t key_index;
    if (identity == NULL || identity->relation_oid !=
                                policy->metadata->relation.relation_oid ||
        identity->key_field_count != policy->key_count ||
        (policy->optimistic_mode != VPS_DML_OPTIMISTIC_OFF &&
         !identity->has_optimistic_field))
        return VPS_DML_MALFORMED_IDENTITY;
    for (key_index = 0U; key_index < policy->key_count; ++key_index) {
        const unsigned char *name;
        size_t length;
        size_t visible = policy->key_visible[key_index];
        uint32_t type_oid = policy->selections[visible].declared_type_oid;
        if (identity->key_fields[key_index].type_oid != type_oid ||
            (key_index != 0U && !vps_dml_append(&plan->query, " AND ", 5U)) ||
            !vps_dml_column_name(policy, visible, &name, &length) ||
            !vps_dml_identifier(&plan->query, name, length) ||
            !vps_dml_append(&plan->query, " IS NOT DISTINCT FROM ", 22U) ||
            !vps_dml_placeholder(&plan->query, plan->parameter_count + 1U) ||
            vps_dml_add_parameter(plan, VPS_DML_PARAMETER_OLD_KEY,
                                  key_index, type_oid) != VPS_DML_OK)
            return VPS_DML_MALFORMED_IDENTITY;
    }
    if (policy->optimistic_mode != VPS_DML_OPTIMISTIC_OFF) {
        uint32_t type_oid;
        if (!vps_dml_append(&plan->query, " AND ", 5U))
            return VPS_DML_OUT_OF_MEMORY;
        if (policy->optimistic_mode == VPS_DML_OPTIMISTIC_XMIN) {
            type_oid = UINT32_C(28);
            if (!vps_dml_append(&plan->query, "xmin=", 5U) ||
                !vps_dml_placeholder(&plan->query,
                                     plan->parameter_count + 1U) ||
                !vps_dml_append(&plan->query, "::pg_catalog.xid", 16U))
                return VPS_DML_OUT_OF_MEMORY;
        } else {
            const unsigned char *name;
            size_t length;
            type_oid = policy->selections[policy->version_visible].declared_type_oid;
            if (!vps_dml_column_name(policy, policy->version_visible,
                                     &name, &length) ||
                !vps_dml_identifier(&plan->query, name, length) ||
                !vps_dml_append(&plan->query, " IS NOT DISTINCT FROM ", 22U) ||
                !vps_dml_placeholder(&plan->query,
                                     plan->parameter_count + 1U))
                return VPS_DML_OUT_OF_MEMORY;
        }
        if (identity->optimistic_field.type_oid != type_oid ||
            vps_dml_add_parameter(plan, VPS_DML_PARAMETER_OLD_OPTIMISTIC,
                                  0U, type_oid) != VPS_DML_OK)
            return VPS_DML_MALFORMED_IDENTITY;
    }
    return VPS_DML_OK;
}

VpsDmlResult vps_dml_plan_build(
    const VpsAllocator *allocator,
    const VpsDmlPolicy *policy,
    VpsDmlOperation operation,
    const unsigned char *included_columns,
    size_t included_count,
    const VpsRowIdentityView *original_identity,
    VpsDmlPlan *plan)
{
    size_t visible;
    size_t write_count = 0U;
    VpsDmlResult result = VPS_DML_OK;
    if (allocator == NULL || !vps_allocator_is_valid(allocator) ||
        policy == NULL || !policy->writable || plan == NULL ||
        operation < VPS_DML_INSERT || operation > VPS_DML_DELETE ||
        (operation != VPS_DML_DELETE &&
         (included_columns == NULL || included_count != policy->visible_count)))
        return VPS_DML_INVALID_ARGUMENT;
    (void)memset(plan, 0, sizeof(*plan));
    if (vps_buffer_init(&plan->query, allocator, VPS_DML_QUERY_LIMIT) !=
        VPS_MEMORY_OK)
        return VPS_DML_OUT_OF_MEMORY;
    plan->initialized = 1;
    plan->operation = operation;
    if (operation == VPS_DML_INSERT) {
        if (!vps_dml_append(&plan->query, "INSERT INTO ", 12U) ||
            !vps_dml_target(&plan->query, policy))
            result = VPS_DML_OUT_OF_MEMORY;
        for (visible = 0U; result == VPS_DML_OK &&
                          visible < policy->visible_count; ++visible) {
            if (!included_columns[visible]) continue;
            if (!policy->insertable[visible]) {
                const VpsColumnMetadata *column =
                    &policy->metadata->columns.columns[
                        policy->visible_to_metadata[visible]];
                result = column->generated_kind != '\0'
                             ? VPS_DML_GENERATED_COLUMN
                             : VPS_DML_IDENTITY_ALWAYS;
                break;
            }
            ++write_count;
        }
        if (result == VPS_DML_OK && write_count == 0U) {
            if (!vps_dml_append(&plan->query, " DEFAULT VALUES", 15U))
                result = VPS_DML_OUT_OF_MEMORY;
        } else if (result == VPS_DML_OK) {
            size_t emitted = 0U;
            if (!vps_dml_append(&plan->query, " (", 2U))
                result = VPS_DML_OUT_OF_MEMORY;
            for (visible = 0U; result == VPS_DML_OK &&
                              visible < policy->visible_count; ++visible) {
                const unsigned char *name;
                size_t length;
                if (!included_columns[visible]) continue;
                if ((emitted++ != 0U && !vps_dml_append(&plan->query, ",", 1U)) ||
                    !vps_dml_column_name(policy, visible, &name, &length) ||
                    !vps_dml_identifier(&plan->query, name, length))
                    result = VPS_DML_OUT_OF_MEMORY;
            }
            if (result == VPS_DML_OK && !vps_dml_append(&plan->query,
                                                         ") VALUES (", 10U))
                result = VPS_DML_OUT_OF_MEMORY;
            emitted = 0U;
            for (visible = 0U; result == VPS_DML_OK &&
                              visible < policy->visible_count; ++visible) {
                if (!included_columns[visible]) continue;
                if ((emitted++ != 0U && !vps_dml_append(&plan->query, ",", 1U)) ||
                    vps_dml_write_value(plan, policy, visible) != VPS_DML_OK)
                    result = VPS_DML_OUT_OF_MEMORY;
            }
            if (result == VPS_DML_OK &&
                !vps_dml_append(&plan->query, ")", 1U))
                result = VPS_DML_OUT_OF_MEMORY;
        }
    } else if (operation == VPS_DML_UPDATE) {
        size_t emitted = 0U;
        if (!vps_dml_append(&plan->query, "UPDATE ", 7U) ||
            !vps_dml_target(&plan->query, policy) ||
            !vps_dml_append(&plan->query, " SET ", 5U))
            result = VPS_DML_OUT_OF_MEMORY;
        for (visible = 0U; result == VPS_DML_OK &&
                          visible < policy->visible_count; ++visible) {
            const unsigned char *name;
            size_t length;
            if (!included_columns[visible]) continue;
            if (!policy->updatable[visible]) {
                const VpsColumnMetadata *column =
                    &policy->metadata->columns.columns[
                        policy->visible_to_metadata[visible]];
                result = column->generated_kind != '\0'
                             ? VPS_DML_GENERATED_COLUMN
                             : VPS_DML_IDENTITY_ALWAYS;
                break;
            }
            if ((emitted++ != 0U && !vps_dml_append(&plan->query, ",", 1U)) ||
                !vps_dml_column_name(policy, visible, &name, &length) ||
                !vps_dml_identifier(&plan->query, name, length) ||
                !vps_dml_append(&plan->query, "=", 1U) ||
                vps_dml_write_value(plan, policy, visible) != VPS_DML_OK)
                result = VPS_DML_OUT_OF_MEMORY;
        }
        if (result == VPS_DML_OK && emitted == 0U)
            result = VPS_DML_INVALID_ARGUMENT;
        if (result == VPS_DML_OK &&
            !vps_dml_append(&plan->query, " WHERE ", 7U))
            result = VPS_DML_OUT_OF_MEMORY;
    } else {
        if (!vps_dml_append(&plan->query, "DELETE FROM ", 12U) ||
            !vps_dml_target(&plan->query, policy) ||
            !vps_dml_append(&plan->query, " WHERE ", 7U))
            result = VPS_DML_OUT_OF_MEMORY;
    }
    if (result == VPS_DML_OK && operation != VPS_DML_INSERT)
        result = vps_dml_original_predicate(plan, policy, original_identity);
    if (result == VPS_DML_OK)
        result = vps_dml_returning(plan, policy);
    if (result == VPS_DML_OK &&
        vps_buffer_append(&plan->query, "\0", 1U) == VPS_MEMORY_OK) {
        plan->query.size -= 1U;
        return VPS_DML_OK;
    }
    if (result == VPS_DML_OK) result = VPS_DML_OUT_OF_MEMORY;
    vps_dml_plan_reset(plan);
    return result;
}

void vps_dml_plan_reset(VpsDmlPlan *plan)
{
    if (plan == NULL) return;
    if (plan->initialized) vps_buffer_reset(&plan->query);
    (void)memset(plan, 0, sizeof(*plan));
}

VpsDmlResult vps_dml_classify_count(VpsDmlOperation operation,
                                    VpsDmlOptimisticMode optimistic_mode,
                                    uint64_t affected_count)
{
    if (operation < VPS_DML_INSERT || operation > VPS_DML_DELETE)
        return VPS_DML_INVALID_ARGUMENT;
    if (affected_count == 1U) return VPS_DML_OK;
    if (affected_count > 1U) return VPS_DML_INVARIANT;
    if (optimistic_mode != VPS_DML_OPTIMISTIC_OFF) return VPS_DML_CONFLICT;
    return operation == VPS_DML_INSERT ? VPS_DML_INVARIANT : VPS_DML_NOT_FOUND;
}

const char *vps_dml_result_name(VpsDmlResult result)
{
    switch (result) {
        case VPS_DML_OK: return "ok";
        case VPS_DML_INVALID_ARGUMENT: return "invalid_argument";
        case VPS_DML_READ_ONLY: return "read_only";
        case VPS_DML_NO_KEY: return "no_key";
        case VPS_DML_UNSUPPORTED_RELATION: return "unsupported_relation";
        case VPS_DML_UNSUPPORTED_TYPE: return "unsupported_type";
        case VPS_DML_GENERATED_COLUMN: return "generated_column";
        case VPS_DML_IDENTITY_ALWAYS: return "identity_always";
        case VPS_DML_OPTIMISTIC_INVALID: return "optimistic_invalid";
        case VPS_DML_MALFORMED_IDENTITY: return "malformed_identity";
        case VPS_DML_LIMIT: return "limit";
        case VPS_DML_OUT_OF_MEMORY: return "out_of_memory";
        case VPS_DML_NOT_FOUND: return "not_found";
        case VPS_DML_CONFLICT: return "conflict";
        case VPS_DML_INVARIANT: return "invariant";
        default: return "unknown";
    }
}

const char *vps_dml_operation_name(VpsDmlOperation operation)
{
    switch (operation) {
        case VPS_DML_INSERT: return "insert";
        case VPS_DML_UPDATE: return "update";
        case VPS_DML_DELETE: return "delete";
        default: return "unknown";
    }
}
