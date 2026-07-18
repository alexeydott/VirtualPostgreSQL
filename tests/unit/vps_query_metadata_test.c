#include "vps_query_metadata.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    (void)fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #condition); \
    ++failures; } } while (0)

static int make_described(VpsQueryDescribeResult *described,
                          const VpsAllocator *allocator,
                          const char *first,
                          const char *second)
{
    if (vps_query_describe_result_init(described, allocator) !=
        VPS_QUERY_VALIDATION_OK) return 0;
    described->field_count = 2U;
    described->allocation_size = 2U * sizeof(*described->fields);
    if (vps_memory_allocate(allocator, described->allocation_size,
                            (void **)&described->fields) != VPS_MEMORY_OK) {
        return 0;
    }
    (void)memset(described->fields, 0, described->allocation_size);
    (void)memcpy(described->fields[0].name, first, strlen(first) + 1U);
    described->fields[0].name_length = strlen(first);
    described->fields[0].type_oid = 23U;
    described->fields[0].type_modifier = -1;
    described->fields[0].origin_relation_oid = 100U;
    described->fields[0].origin_attribute_number = 1;
    (void)memcpy(described->fields[1].name, second, strlen(second) + 1U);
    described->fields[1].name_length = strlen(second);
    described->fields[1].type_oid = 25U;
    described->fields[1].type_modifier = -1;
    return 1;
}

int main(void)
{
    VpsAllocator allocator;
    VpsQueryDescribeResult described;
    VpsQueryResultMetadata metadata;
    VpsQueryMetadataPolicy policy;
    VpsQueryColumnPolicy columns[2];

    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    CHECK(make_described(&described, &allocator, "Id", "Category") != 0);
    CHECK(vps_query_result_metadata_init(&metadata, &allocator, NULL) ==
          VPS_QUERY_METADATA_OK);
    (void)memset(&policy, 0, sizeof(policy));
    (void)memset(columns, 0, sizeof(columns));
    columns[1].collation_oid = 100U;
    policy.columns = columns;
    policy.column_count = 2U;
    policy.key_columns = "id";
    policy.key_columns_length = 2U;
    policy.query_indexes = "by_id=ID;by_category=category";
    policy.query_indexes_length = strlen(policy.query_indexes);
    policy.materialization = VPS_QUERY_MATERIALIZATION_MEMORY;
    CHECK(vps_query_result_metadata_build(&metadata, &described, &policy) ==
          VPS_QUERY_METADATA_OK);
    CHECK(metadata.key_column_count == 1U && metadata.key_columns[0] == 0U);
    CHECK(metadata.indexes.index_count == 2U);
    CHECK(metadata.columns[1].collation_oid == 100U);
    CHECK(metadata.materialization == VPS_QUERY_MATERIALIZATION_MEMORY);
    vps_query_result_metadata_cleanup(&metadata);
    vps_query_describe_result_cleanup(&described);

    CHECK(make_described(&described, &allocator, "Alias", "alias") != 0);
    CHECK(vps_query_result_metadata_init(&metadata, &allocator, NULL) ==
          VPS_QUERY_METADATA_OK);
    (void)memset(&policy, 0, sizeof(policy));
    CHECK(vps_query_result_metadata_build(&metadata, &described, &policy) ==
          VPS_QUERY_METADATA_DUPLICATE_ALIAS);
    vps_query_result_metadata_cleanup(&metadata);
    vps_query_describe_result_cleanup(&described);

    CHECK(make_described(&described, &allocator, "id", "category") != 0);
    CHECK(vps_query_result_metadata_init(&metadata, &allocator, NULL) ==
          VPS_QUERY_METADATA_OK);
    (void)memset(&policy, 0, sizeof(policy));
    policy.key_columns = "missing";
    policy.key_columns_length = 7U;
    CHECK(vps_query_result_metadata_build(&metadata, &described, &policy) ==
          VPS_QUERY_METADATA_UNKNOWN_KEY_COLUMN);
    vps_query_result_metadata_cleanup(&metadata);
    vps_query_describe_result_cleanup(&described);

    CHECK(make_described(&described, &allocator, "id", "category") != 0);
    {
        VpsQueryIndexSet indexes;
        CHECK(vps_query_indexes_parse("same=id;same=category", 21U,
                                      &described, &indexes) ==
              VPS_QUERY_INDEXES_DUPLICATE_NAME);
        CHECK(vps_query_indexes_parse("dup=id,id", 9U, &described,
                                      &indexes) ==
              VPS_QUERY_INDEXES_DUPLICATE_COLUMN);
        CHECK(vps_query_indexes_parse("bad=missing", 11U, &described,
                                      &indexes) ==
              VPS_QUERY_INDEXES_UNKNOWN_COLUMN);
        CHECK(vps_query_indexes_parse("expr=id+1", 9U, &described,
                                      &indexes) ==
              VPS_QUERY_INDEXES_INVALID_SYNTAX);
    }
    vps_query_describe_result_cleanup(&described);

    if (failures != 0) return 1;
    (void)puts("vps_query_metadata_test: passed");
    return 0;
}
