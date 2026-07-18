#include "vps_schema_fingerprint.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
                          #condition);                                         \
            goto cleanup;                                                      \
        }                                                                      \
    } while (0)

static int add_text(VpsColumnSet *columns,
                    VpsMetadataString *string,
                    const char *value)
{
    size_t length = strlen(value);
    string->offset = columns->text.size;
    string->length = length;
    string->present = 1;
    return vps_buffer_append(&columns->text, value, length) == VPS_MEMORY_OK;
}

int main(void)
{
    static const char expected_vector[] =
        "f563dd9d6933bec457da41936aad0175aa75b5427d8cb95ce7f9454aa3b6821b";
    VpsAllocator allocator;
    VpsRelationMetadata relation;
    VpsColumnSet columns;
    VpsColumnMetadata *column;
    VpsKeyMetadata key;
    VpsRelationPolicyMetadata policy;
    VpsTypeRegistry registry;
    VpsSchemaFingerprintInput input;
    VpsSchemaFingerprint baseline;
    VpsSchemaFingerprint repeated;
    VpsSchemaFingerprint changed;
    int relation_initialized = 0;
    int columns_initialized = 0;
    int passed = 0;
    (void)memset(&relation, 0, sizeof(relation));
    (void)memset(&columns, 0, sizeof(columns));
    (void)memset(&key, 0, sizeof(key));
    (void)memset(&policy, 0, sizeof(policy));
    (void)memset(&input, 0, sizeof(input));
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    CHECK(vps_relation_metadata_init(&relation, &allocator, NULL) ==
          VPS_METADATA_OK);
    relation_initialized = 1;
    relation.namespace_oid = 2200U;
    relation.relation_oid = 4242U;
    relation.access_method_oid = 2U;
    relation.kind = VPS_RELATION_TABLE;
    relation.persistence = 'p';
    relation.readable = 1;
    relation.writable_candidate = 1;
    CHECK(vps_buffer_append(&relation.schema_name, "public", 6U) ==
          VPS_MEMORY_OK);
    CHECK(vps_buffer_append(&relation.relation_name, "fixture", 7U) ==
          VPS_MEMORY_OK);
    CHECK(vps_column_set_init(&columns, &allocator, NULL) == VPS_METADATA_OK);
    columns_initialized = 1;
    CHECK(vps_memory_allocate(&allocator, sizeof(*column),
                              (void **)&columns.columns) == VPS_MEMORY_OK);
    columns.columns_bytes = sizeof(*column);
    columns.column_count = 1U;
    columns.visible_count = 1U;
    column = columns.columns;
    (void)memset(column, 0, sizeof(*column));
    column->attribute_number = 1;
    column->type_oid = 23U;
    column->type_modifier = -1;
    column->type_namespace_oid = 11U;
    column->type_category = 'N';
    column->type_kind = 'b';
    column->not_null = 1;
    column->origin_relation_oid = relation.relation_oid;
    column->origin_attribute_number = 1;
    column->statistics_target = -1;
    CHECK(add_text(&columns, &column->name, "id"));
    CHECK(add_text(&columns, &column->type_namespace, "pg_catalog"));
    CHECK(add_text(&columns, &column->type_name, "int4"));
    key.source = VPS_KEY_PRIMARY;
    key.index_oid = 4243U;
    key.attribute_numbers[0] = 1;
    key.column_count = 1U;
    policy.write_policy = VPS_RELATION_WRITE_ALLOWED;
    CHECK(vps_type_registry_init(&registry, NULL) == VPS_METADATA_OK);
    input.relation = &relation;
    input.columns = &columns;
    input.key = &key;
    input.policy = &policy;
    input.type_registry = &registry;
    input.spatial.metadata_version = 1U;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &baseline) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_build(&input, NULL, &repeated) ==
          VPS_METADATA_OK);
    CHECK(memcmp(baseline.bytes, repeated.bytes,
                 VPS_SCHEMA_FINGERPRINT_BYTES) == 0);
    CHECK(vps_schema_fingerprint_compare(&baseline, &repeated) ==
          VPS_SCHEMA_CHANGE_NONE);

    column->statistics_target = 100;
    column->has_comment = 1;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_NONE);
    column->statistics_target = -1;
    column->has_comment = 0;

    column->type_modifier = 12;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_COLUMN_LAYOUT);
    column->type_modifier = -1;
    CHECK(add_text(&columns, &column->default_expression_hash,
                   "0123456789abcdef0123456789abcdef"));
    column->has_default = 1;
    column->default_kind = 'u';
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_COLUMN_LAYOUT);
    column->default_expression_hash.present = 0;
    column->has_default = 0;
    column->default_kind = '\0';
    CHECK(add_text(&columns, &column->domain_constraint_hash,
                   "fedcba9876543210fedcba9876543210"));
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_COLUMN_LAYOUT);
    column->domain_constraint_hash.present = 0;
    column->generated_kind = 's';
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_COLUMN_LAYOUT);
    column->generated_kind = '\0';
    column->collation_oid = 100U;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_COLUMN_LAYOUT);
    column->collation_oid = 0U;
    key.index_oid += 1U;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_KEY);
    key.index_oid -= 1U;
    policy.row_security = 1;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_RELATION_POLICY);
    policy.row_security = 0;
    policy.partition_strategy = 'r';
    policy.partition_attribute_numbers[0] = 1;
    policy.partition_attribute_count = 1U;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_RELATION_POLICY);
    policy.partition_strategy = '\0';
    policy.partition_attribute_count = 0U;
    registry.version += 1U;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_INVALID_ARGUMENT);
    registry.version -= 1U;
    changed = baseline;
    changed.bytes[0] ^= 1U;
    changed.codec_hash ^= UINT64_C(1);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_CODEC_REGISTRY);
    input.spatial.extension_namespace_oid = 2200U;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_SPATIAL);
    input.spatial.extension_namespace_oid = 0U;
    input.spatial.srid = 4326;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_SPATIAL);
    input.spatial.srid = 0;
    relation.relation_oid += 1U;
    CHECK(vps_schema_fingerprint_build(&input, NULL, &changed) ==
          VPS_METADATA_OK);
    CHECK(vps_schema_fingerprint_compare(&baseline, &changed) ==
          VPS_SCHEMA_CHANGE_RELATION_IDENTITY);
    relation.relation_oid -= 1U;
    CHECK(vps_schema_fingerprint_compare(NULL, &baseline) ==
          VPS_SCHEMA_CHANGE_UNKNOWN);
    CHECK(strlen(baseline.hex) == VPS_SCHEMA_FINGERPRINT_HEX_LENGTH);
    (void)printf("schema_fingerprint_vector version=%u value=%s\n",
                 (unsigned int)baseline.version, baseline.hex);
    CHECK(strcmp(baseline.hex, expected_vector) == 0);
    passed = 1;

cleanup:
    if (columns_initialized) vps_column_set_reset(&columns);
    if (relation_initialized) vps_relation_metadata_reset(&relation);
    return passed ? 0 : 1;
}
