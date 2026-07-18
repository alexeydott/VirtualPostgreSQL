#include "vps_dml.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) {                           \
    (void)fprintf(stderr, "check failed at line %d: %s\n", __LINE__,       \
                  #condition); goto cleanup; } } while (0)

static int add_string(VpsColumnSet *set, VpsMetadataString *string,
                      const char *value)
{
    size_t length = strlen(value);
    string->offset = set->text.size;
    string->length = length;
    string->present = 1;
    return vps_buffer_append(&set->text, value, length) == VPS_MEMORY_OK;
}

static int add_column(VpsColumnSet *set, size_t index, const char *name,
                      uint32_t oid, const char *type_name, char category,
                      int not_null)
{
    VpsColumnMetadata *column = &set->columns[index];
    (void)memset(column, 0, sizeof(*column));
    column->attribute_number = (int32_t)(index + 1U);
    column->type_oid = oid;
    column->type_modifier = -1;
    column->type_namespace_oid = 11U;
    column->type_category = category;
    column->type_kind = 'b';
    column->not_null = not_null;
    column->origin_relation_oid = 4242U;
    column->origin_attribute_number = column->attribute_number;
    column->statistics_target = -1;
    return add_string(set, &column->name, name) &&
           add_string(set, &column->type_namespace, "pg_catalog") &&
           add_string(set, &column->type_name, type_name);
}

int main(void)
{
    VpsAllocator allocator;
    VpsTableMetadata metadata;
    VpsDmlPolicy policy;
    VpsDmlPlan plan;
    VpsRowIdentityField keys[1];
    VpsRowIdentityField version;
    VpsRowIdentitySpec identity_spec;
    VpsRowIdentityView identity;
    VpsBuffer token;
    unsigned char included[4] = {1U, 1U, 1U, 1U};
    int relation_initialized = 0;
    int columns_initialized = 0;
    int token_initialized = 0;
    int passed = 0;
    (void)memset(&metadata, 0, sizeof(metadata));
    (void)memset(&plan, 0, sizeof(plan));
    (void)memset(&identity, 0, sizeof(identity));
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    CHECK(vps_relation_metadata_init(&metadata.relation, &allocator, NULL) ==
          VPS_METADATA_OK);
    relation_initialized = 1;
    metadata.relation.relation_oid = 4242U;
    metadata.relation.kind = VPS_RELATION_TABLE;
    metadata.relation.readable = 1;
    metadata.relation.writable_candidate = 1;
    CHECK(vps_buffer_append(&metadata.relation.schema_name, "odd\"schema", 10U)
          == VPS_MEMORY_OK);
    CHECK(vps_buffer_append(&metadata.relation.relation_name, "dml_fixture", 11U)
          == VPS_MEMORY_OK);
    CHECK(vps_column_set_init(&metadata.columns, &allocator, NULL) ==
          VPS_METADATA_OK);
    columns_initialized = 1;
    CHECK(vps_memory_allocate(&allocator,
                              4U * sizeof(*metadata.columns.columns),
                              (void **)&metadata.columns.columns) ==
          VPS_MEMORY_OK);
    metadata.columns.columns_bytes = 4U * sizeof(*metadata.columns.columns);
    metadata.columns.column_count = 4U;
    metadata.columns.visible_count = 4U;
    CHECK(add_column(&metadata.columns, 0U, "id", 23U, "int4", 'N', 1));
    CHECK(add_column(&metadata.columns, 1U, "payload", 25U, "text", 'S', 0));
    CHECK(add_column(&metadata.columns, 2U, "bytes", 17U, "bytea", 'U', 0));
    CHECK(add_column(&metadata.columns, 3U, "version", 20U, "int8", 'N', 1));
    metadata.key.source = VPS_KEY_PRIMARY;
    metadata.key.column_count = 1U;
    metadata.key.attribute_numbers[0] = 1;
    metadata.policy.write_policy = VPS_RELATION_WRITE_ALLOWED;
    CHECK(vps_type_registry_init(&metadata.type_registry, NULL) ==
          VPS_METADATA_OK);
    metadata.loaded = 1;

    CHECK(vps_dml_policy_build(&metadata, 1, VPS_DML_OPTIMISTIC_COLUMN,
                               "version", 7U, &policy) == VPS_DML_OK);
    included[0] = 0U;
    included[3] = 0U;
    CHECK(vps_dml_plan_build(&allocator, &policy, VPS_DML_INSERT, included,
                             4U, NULL, &plan) == VPS_DML_OK);
    CHECK(strstr((const char *)plan.query.data,
                 "INSERT INTO \"odd\"\"schema\".\"dml_fixture\"") != NULL);
    CHECK(strstr((const char *)plan.query.data, "\"payload\",\"bytes\"") != NULL);
    CHECK(plan.parameter_count == 2U && plan.returning_count == 2U);
    vps_dml_plan_reset(&plan);

    (void)memset(keys, 0, sizeof(keys));
    (void)memset(&version, 0, sizeof(version));
    (void)memset(&identity_spec, 0, sizeof(identity_spec));
    keys[0].kind = VPS_ROW_IDENTITY_FIELD_TEXT;
    keys[0].type_oid = 23U;
    keys[0].bytes = "7";
    keys[0].length = 1U;
    version.kind = VPS_ROW_IDENTITY_FIELD_TEXT;
    version.type_oid = 20U;
    version.bytes = "11";
    version.length = 2U;
    identity_spec.relation_oid = 4242U;
    identity_spec.key_fields = keys;
    identity_spec.key_field_count = 1U;
    identity_spec.optimistic_field = &version;
    CHECK(vps_row_identity_encode(&allocator, &identity_spec, &token) ==
          VPS_ROW_IDENTITY_OK);
    token_initialized = 1;
    CHECK(vps_row_identity_decode(token.data, token.size, &identity) ==
          VPS_ROW_IDENTITY_OK);
    (void)memset(included, 0, sizeof(included));
    included[0] = 1U;
    included[1] = 1U;
    CHECK(vps_dml_plan_build(&allocator, &policy, VPS_DML_UPDATE, included,
                             4U, &identity, &plan) == VPS_DML_OK);
    CHECK(strstr((const char *)plan.query.data,
                 "WHERE \"id\" IS NOT DISTINCT FROM $3") != NULL);
    CHECK(strstr((const char *)plan.query.data,
                 "\"version\" IS NOT DISTINCT FROM $4") != NULL);
    CHECK(plan.parameter_count == 4U && plan.returns_optimistic);
    vps_dml_plan_reset(&plan);

    CHECK(vps_dml_plan_build(&allocator, &policy, VPS_DML_DELETE, NULL, 0U,
                             &identity, &plan) == VPS_DML_OK);
    CHECK(strstr((const char *)plan.query.data, "DELETE FROM") != NULL);
    CHECK(plan.parameter_count == 2U);
    vps_dml_plan_reset(&plan);
    CHECK(vps_dml_classify_count(VPS_DML_UPDATE,
                                 VPS_DML_OPTIMISTIC_COLUMN, 0U) ==
          VPS_DML_CONFLICT);
    CHECK(vps_dml_classify_count(VPS_DML_DELETE,
                                 VPS_DML_OPTIMISTIC_OFF, 0U) ==
          VPS_DML_NOT_FOUND);
    CHECK(vps_dml_classify_count(VPS_DML_UPDATE,
                                 VPS_DML_OPTIMISTIC_OFF, 2U) ==
          VPS_DML_INVARIANT);
    identity.relation_oid += 1U;
    CHECK(vps_dml_plan_build(&allocator, &policy, VPS_DML_DELETE, NULL, 0U,
                             &identity, &plan) ==
          VPS_DML_MALFORMED_IDENTITY);
    passed = 1;
cleanup:
    vps_dml_plan_reset(&plan);
    if (token_initialized) vps_buffer_reset(&token);
    if (columns_initialized) vps_column_set_reset(&metadata.columns);
    if (relation_initialized) vps_relation_metadata_reset(&metadata.relation);
    if (passed) (void)printf("vps_dml_test: passed\n");
    return passed ? 0 : 1;
}
