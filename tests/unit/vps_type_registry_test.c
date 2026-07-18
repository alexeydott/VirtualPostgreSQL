#include "vps_type_registry.h"

#include <stdio.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                           \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "type_registry_test=%s status=failed line=%d\n", \
                          name, __LINE__);                                    \
            return 0;                                                         \
        }                                                                     \
    } while (0)

static int test_add_string(VpsColumnSet *set,
                           VpsMetadataString *target,
                           const char *value)
{
    size_t length = strlen(value);
    target->offset = set->text.size;
    target->length = length;
    target->present = 1;
    return vps_buffer_append(&set->text, value, length) == VPS_MEMORY_OK;
}

static int test_registry_matrix(void)
{
    VpsAllocator allocator;
    VpsColumnSet columns;
    VpsTypeRegistry registry;
    VpsTypeSelection selection;
    size_t bytes;
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   vps_column_set_init(&columns, &allocator, NULL) ==
                       VPS_METADATA_OK &&
                   vps_type_registry_init(&registry, NULL) == VPS_METADATA_OK &&
                   vps_size_multiply(5U, sizeof(VpsColumnMetadata), &bytes) ==
                       VPS_MEMORY_OK &&
                   vps_memory_allocate(&allocator, bytes,
                                       (void **)&columns.columns) ==
                       VPS_MEMORY_OK,
               "registry_init");
    columns.columns_bytes = bytes;
    columns.column_count = 5U;
    columns.visible_count = 5U;
    (void)memset(columns.columns, 0, bytes);

    columns.columns[0].type_oid = 23U;
    columns.columns[0].type_kind = 'b';
    columns.columns[0].type_category = 'N';
    TEST_CHECK(test_add_string(&columns,
                               &columns.columns[0].type_namespace,
                               "pg_catalog") &&
                   test_add_string(&columns, &columns.columns[0].type_name,
                                   "int4") &&
                   vps_type_registry_select(&registry, &columns, 0U,
                                            &selection) == VPS_METADATA_OK &&
                   selection.codec == VPS_CODEC_INTEGER &&
                   (selection.capabilities & VPS_CODEC_CAP_PUSHDOWN_EXACT) != 0U,
               "registry_int4");

    columns.columns[1].type_oid = 50000U;
    columns.columns[1].type_kind = 'd';
    columns.columns[1].type_category = 'N';
    columns.columns[1].domain_base_oid = 1700U;
    columns.columns[1].domain_base_kind = 'b';
    columns.columns[1].domain_base_category = 'N';
    TEST_CHECK(test_add_string(&columns,
                               &columns.columns[1].type_namespace,
                               "public") &&
                   test_add_string(&columns, &columns.columns[1].type_name,
                                   "money_domain") &&
                   test_add_string(&columns,
                                   &columns.columns[1].domain_base_namespace,
                                   "pg_catalog") &&
                   test_add_string(&columns,
                                   &columns.columns[1].domain_base_name,
                                   "numeric") &&
                   vps_type_registry_select(&registry, &columns, 1U,
                                            &selection) == VPS_METADATA_OK &&
                   selection.domain && selection.effective_type_oid == 1700U &&
                   selection.codec == VPS_CODEC_NUMERIC_TEXT &&
                   (selection.capabilities & VPS_CODEC_CAP_ORDER) == 0U,
               "registry_domain");

    columns.columns[2].type_oid = 50001U;
    columns.columns[2].type_kind = 'e';
    columns.columns[2].type_category = 'E';
    TEST_CHECK(test_add_string(&columns,
                               &columns.columns[2].type_namespace,
                               "public") &&
                   test_add_string(&columns, &columns.columns[2].type_name,
                                   "status_enum") &&
                   vps_type_registry_select(&registry, &columns, 2U,
                                            &selection) == VPS_METADATA_OK &&
                   selection.codec == VPS_CODEC_ENUM_TEXT &&
                   (selection.capabilities & VPS_CODEC_CAP_DML) != 0U,
               "registry_enum");

    columns.columns[3].type_oid = 50002U;
    columns.columns[3].type_kind = 'b';
    columns.columns[3].type_category = 'U';
    TEST_CHECK(test_add_string(&columns,
                               &columns.columns[3].type_namespace,
                               "extension_schema") &&
                   test_add_string(&columns, &columns.columns[3].type_name,
                                   "custom_type") &&
                   vps_type_registry_select(&registry, &columns, 3U,
                                            &selection) == VPS_METADATA_OK &&
                   selection.codec == VPS_CODEC_USER_TEXT &&
                   selection.capabilities == VPS_CODEC_CAP_READ,
               "registry_fallback");

    columns.columns[4].type_oid = 1007U;
    columns.columns[4].type_kind = 'b';
    columns.columns[4].type_category = 'A';
    TEST_CHECK(test_add_string(&columns,
                               &columns.columns[4].type_namespace,
                               "pg_catalog") &&
                   test_add_string(&columns, &columns.columns[4].type_name,
                                   "_int4") &&
                   vps_type_registry_select(&registry, &columns, 4U,
                                            &selection) == VPS_METADATA_OK &&
                   selection.codec == VPS_CODEC_ARRAY_TEXT,
               "registry_array");

    vps_column_set_reset(&columns);
    return 1;
}

int main(void)
{
    if (!test_registry_matrix()) return 1;
    (void)printf("type_registry_tests=passed\n");
    return 0;
}
