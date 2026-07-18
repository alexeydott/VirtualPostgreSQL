#include "vps_libpq_client_metadata.h"
#include "vps_metadata.h"

#include <stdio.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                           \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "metadata_test=%s status=failed line=%d\n", \
                          name, __LINE__);                                    \
            return 0;                                                         \
        }                                                                     \
    } while (0)

typedef struct TestInput {
    const char *values[2][VPS_METADATA_MAX_FIELDS];
    size_t rows;
    size_t fields;
} TestInput;

static int test_input_is_null(void *context, size_t row, size_t field)
{
    TestInput *input = (TestInput *)context;
    return input->values[row][field] == NULL;
}

static const void *test_input_value(void *context, size_t row, size_t field)
{
    TestInput *input = (TestInput *)context;
    return input->values[row][field];
}

static size_t test_input_length(void *context, size_t row, size_t field)
{
    TestInput *input = (TestInput *)context;
    const char *value = input->values[row][field];
    return value == NULL ? 0U : strlen(value);
}

static int test_query_specs(void)
{
    VpsCatalogQuery query;
    for (query = VPS_CATALOG_QUERY_RELATION;
         query < VPS_CATALOG_QUERY_COUNT;
         query = (VpsCatalogQuery)((int)query + 1)) {
        VpsCatalogQuerySpec spec;
        TEST_CHECK(vps_metadata_catalog_query_spec(query, &spec) ==
                       VPS_METADATA_OK &&
                       spec.sql != NULL && spec.sql_length == strlen(spec.sql) &&
                       strstr(spec.sql, "pg_catalog.pg_") != NULL &&
                       strstr(spec.sql, " WHERE ") != NULL &&
                       strstr(spec.sql, "$1::pg_catalog.") != NULL &&
                       strstr(spec.sql, " information_schema.") == NULL &&
                       spec.parameter_count >= 1U &&
                       spec.result_field_count >= 1U,
                   vps_catalog_query_name(query));
    }
    TEST_CHECK(vps_metadata_catalog_query_spec(VPS_CATALOG_QUERY_COUNT, NULL) ==
                   VPS_METADATA_INVALID_ARGUMENT,
               "query_invalid");
    return 1;
}

static int test_rowset_copy_and_atomicity(void)
{
    VpsAllocator allocator;
    VpsMetadataRowSet rowset;
    VpsCatalogQuerySpec spec;
    VpsMetadataInput input;
    TestInput source;
    const unsigned char *value;
    size_t length;
    int is_null;
    size_t field;
    (void)memset(&source, 0, sizeof(source));
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   vps_metadata_rowset_init(&rowset, &allocator, NULL) ==
                       VPS_METADATA_OK &&
                   vps_metadata_catalog_query_spec(VPS_CATALOG_QUERY_RELATION,
                                                   &spec) == VPS_METADATA_OK,
               "rowset_init");
    source.rows = 1U;
    source.fields = spec.result_field_count;
    for (field = 0U; field < source.fields; ++field) source.values[0][field] = "1";
    source.values[0][3] = NULL;
    input.context = &source;
    input.row_count = source.rows;
    input.field_count = source.fields;
    input.is_null = test_input_is_null;
    input.value = test_input_value;
    input.length = test_input_length;
    TEST_CHECK(vps_metadata_rowset_copy(&rowset, VPS_CATALOG_QUERY_RELATION,
                                        &input) == VPS_METADATA_OK &&
                   rowset.row_count == 1U &&
                   rowset.field_count == spec.result_field_count &&
                   vps_metadata_rowset_cell(&rowset, 0U, 0U, &value, &length,
                                            &is_null) == VPS_METADATA_OK &&
                   !is_null && length == 1U && value[0] == '1' &&
                   vps_metadata_rowset_cell(&rowset, 0U, 3U, &value, &length,
                                            &is_null) == VPS_METADATA_OK &&
                   is_null && value == NULL && length == 0U,
               "rowset_copy");
    source.values[0][0] = "2";
    TEST_CHECK(vps_metadata_rowset_append(&rowset,
                                          VPS_CATALOG_QUERY_RELATION,
                                          &input) == VPS_METADATA_OK &&
                   rowset.row_count == 2U &&
                   vps_metadata_rowset_cell(&rowset, 1U, 0U, &value, &length,
                                            &is_null) == VPS_METADATA_OK &&
                   !is_null && length == 1U && value[0] == '2',
               "rowset_append");
    input.field_count -= 1U;
    TEST_CHECK(vps_metadata_rowset_copy(&rowset, VPS_CATALOG_QUERY_RELATION,
                                        &input) ==
                       VPS_METADATA_INVALID_RESULT &&
                   rowset.row_count == 2U,
               "rowset_atomic_invalid");
    vps_metadata_rowset_reset(&rowset);
    vps_metadata_rowset_reset(&rowset);
    TEST_CHECK(rowset.initialized && rowset.cells == NULL && rowset.bytes == NULL,
               "rowset_repeat_reset");
    return 1;
}

static int test_statement_contract(void)
{
    static const char schema[] = "public";
    static const char relation[] = "fixture_rows";
    VpsClientParameterView parameters[2];
    VpsLibpqMetadataStatement statement;
    VpsCatalogQuerySpec catalog;
    (void)memset(parameters, 0, sizeof(parameters));
    parameters[0].value = schema;
    parameters[0].length = sizeof(schema) - 1U;
    parameters[0].type_oid = VPS_METADATA_NAME_OID;
    parameters[0].format = VPS_CLIENT_VALUE_TEXT;
    parameters[1].value = relation;
    parameters[1].length = sizeof(relation) - 1U;
    parameters[1].type_oid = VPS_METADATA_NAME_OID;
    parameters[1].format = VPS_CLIENT_VALUE_TEXT;
    TEST_CHECK(vps_metadata_catalog_query_spec(VPS_CATALOG_QUERY_RELATION,
                                               &catalog) == VPS_METADATA_OK &&
                   vps_libpq_metadata_statement_init(
                       &statement, VPS_CATALOG_QUERY_RELATION, parameters, 2U,
                       5000U) == VPS_METADATA_OK &&
                   statement.statement.query == catalog.sql &&
                   statement.statement.parameter_count == 2U &&
                   statement.statement.result_field_count ==
                       catalog.result_field_count &&
                   statement.statement.single_row &&
                   !statement.statement.prepare &&
                   statement.result_fields[0].type_oid ==
                       VPS_METADATA_TEXT_OID,
               "statement_valid");
    parameters[0].is_null = 1;
    TEST_CHECK(vps_libpq_metadata_statement_init(
                   &statement, VPS_CATALOG_QUERY_RELATION, parameters, 2U,
                   5000U) == VPS_METADATA_INVALID_ARGUMENT,
               "statement_null_filter");
    return 1;
}

static int test_relation_resolution(void)
{
    VpsAllocator allocator;
    VpsMetadataRowSet rowset;
    VpsRelationMetadata relation;
    VpsMetadataInput input;
    TestInput source;
    const char *table_values[12] = {
        "2200", "41000", "r", "p", "f", "f", "f", "f", "f",
        "CaseSchema", "CaseTable", "2"};
    size_t field;
    (void)memset(&source, 0, sizeof(source));
    source.rows = 1U;
    source.fields = 12U;
    for (field = 0U; field < 12U; ++field)
        source.values[0][field] = table_values[field];
    input.context = &source;
    input.row_count = 1U;
    input.field_count = 12U;
    input.is_null = test_input_is_null;
    input.value = test_input_value;
    input.length = test_input_length;
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   vps_metadata_rowset_init(&rowset, &allocator, NULL) ==
                       VPS_METADATA_OK &&
                   vps_relation_metadata_init(&relation, &allocator, NULL) ==
                       VPS_METADATA_OK &&
                   vps_metadata_rowset_copy(&rowset,
                                             VPS_CATALOG_QUERY_RELATION,
                                             &input) == VPS_METADATA_OK &&
                   vps_relation_metadata_resolve(
                       &relation, &rowset, "CaseSchema", 10U, "CaseTable",
                       9U) == VPS_METADATA_OK &&
                   relation.namespace_oid == 2200U &&
                   relation.relation_oid == 41000U &&
                   relation.kind == VPS_RELATION_TABLE && relation.readable &&
                   relation.writable_candidate,
               "relation_table");
    TEST_CHECK(vps_relation_metadata_resolve(
                   &relation, &rowset, "caseschema", 10U, "CaseTable", 9U) ==
                   VPS_METADATA_INVALID_RESULT,
               "relation_exact_case");
    source.values[0][2] = "S";
    TEST_CHECK(vps_metadata_rowset_copy(&rowset,
                                        VPS_CATALOG_QUERY_RELATION, &input) ==
                       VPS_METADATA_OK &&
                   vps_relation_metadata_resolve(
                       &relation, &rowset, "CaseSchema", 10U, "CaseTable",
                       9U) == VPS_METADATA_UNSUPPORTED,
               "relation_sequence_rejected");
    source.values[0][2] = "r";
    source.values[0][4] = "t";
    source.values[0][8] = "t";
    TEST_CHECK(vps_metadata_rowset_copy(&rowset,
                                        VPS_CATALOG_QUERY_RELATION, &input) ==
                       VPS_METADATA_OK &&
                   vps_relation_metadata_resolve(
                       &relation, &rowset, "CaseSchema", 10U, "CaseTable",
                       9U) == VPS_METADATA_OK &&
                   relation.kind == VPS_RELATION_PARTITION &&
                   relation.has_parent,
               "relation_partition");
    input.row_count = 0U;
    source.rows = 0U;
    TEST_CHECK(vps_metadata_rowset_copy(&rowset,
                                        VPS_CATALOG_QUERY_RELATION, &input) ==
                       VPS_METADATA_OK &&
                   vps_relation_metadata_resolve(
                       &relation, &rowset, "CaseSchema", 10U, "CaseTable",
                       9U) == VPS_METADATA_NOT_FOUND,
               "relation_missing");
    vps_relation_metadata_reset(&relation);
    vps_metadata_rowset_reset(&rowset);
    return 1;
}

static int test_column_metadata(void)
{
    static const char *first[39] = {
        "1", "id", "23", "-1", "11", "pg_catalog", "int4", "N",
        "b", "0", "-1", "0", "t", "f", "t", "", "a", "0",
        NULL, NULL, NULL, "p", "", "-1", "f", "41000", "1", "f",
        NULL, NULL, NULL, NULL, NULL, "0123456789abcdef0123456789abcdef",
        "s", "f", NULL, NULL, "integer"};
    static const char *second[39] = {
        "2", "amount", "50000", "-1", "2200", "public", "money_domain",
        "N", "d", "1700", "-1", "0", "f", "t", "f", "s", "", "100",
        "C", "c", "t", "m", "p", "10", "t", "41000", "2", "t",
        "11", "pg_catalog", "numeric", "N", "b", NULL,
        "g", "t", "fedcba9876543210fedcba9876543210",
        "00112233445566778899aabbccddeeff", "public.money_domain"};
    VpsAllocator allocator;
    VpsMetadataRowSet rowset;
    VpsColumnSet columns;
    VpsMetadataInput input;
    TestInput source;
    const unsigned char *name;
    size_t name_length;
    size_t field;
    (void)memset(&source, 0, sizeof(source));
    source.rows = 2U;
    source.fields = 39U;
    for (field = 0U; field < 39U; ++field) {
        source.values[0][field] = first[field];
        source.values[1][field] = second[field];
    }
    input.context = &source;
    input.row_count = 2U;
    input.field_count = 39U;
    input.is_null = test_input_is_null;
    input.value = test_input_value;
    input.length = test_input_length;
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   vps_metadata_rowset_init(&rowset, &allocator, NULL) ==
                       VPS_METADATA_OK &&
                   vps_column_set_init(&columns, &allocator, NULL) ==
                       VPS_METADATA_OK &&
                   vps_metadata_rowset_copy(&rowset,
                                             VPS_CATALOG_QUERY_COLUMNS,
                                             &input) == VPS_METADATA_OK &&
                   vps_column_set_build(&columns, &rowset) == VPS_METADATA_OK,
               "columns_build");
    TEST_CHECK(columns.column_count == 2U && columns.visible_count == 1U &&
                   columns.columns[0].attribute_number == 1 &&
                   columns.columns[0].type_oid == 23U &&
                   columns.columns[0].identity_kind == 'a' &&
                   columns.columns[0].default_kind == 's' &&
                   columns.columns[0].collation_provider == '\0' &&
                   columns.columns[1].dropped &&
                   columns.columns[1].type_kind == 'd' &&
                   columns.columns[1].domain_base_oid == 1700U &&
                   columns.columns[1].domain_not_null &&
                   columns.columns[1].domain_has_default &&
                   columns.columns[1].generated_kind == 's' &&
                   columns.columns[1].collation_provider == 'c' &&
                   columns.columns[1].has_comment,
               "columns_fields");
    TEST_CHECK(vps_column_set_string(&columns, &columns.columns[1].name,
                                     &name, &name_length) == VPS_METADATA_OK &&
                   name_length == 6U && memcmp(name, "amount", 6U) == 0,
               "columns_string");
    source.values[1][3] = "2147483648";
    TEST_CHECK(vps_metadata_rowset_copy(&rowset,
                                        VPS_CATALOG_QUERY_COLUMNS, &input) ==
                       VPS_METADATA_OK &&
                   vps_column_set_build(&columns, &rowset) ==
                       VPS_METADATA_INVALID_RESULT &&
                   columns.column_count == 2U,
               "columns_atomic_invalid");
    vps_column_set_reset(&columns);
    vps_metadata_rowset_reset(&rowset);
    return 1;
}

static int test_key_discovery(void)
{
    static const char *partial[16] = {
        "50001", "f", "t", "t", "t", "t", "t", "f", "f", "1",
        "1", "1", "f", NULL, "btree", "partial_key"};
    static const char *primary[16] = {
        "50002", "t", "t", "t", "t", "t", "f", "f", "f", "2",
        "3", "1 2 3", "f", "p", "btree", "primary_key"};
    VpsAllocator allocator;
    VpsMetadataRowSet rows;
    VpsColumnSet columns;
    VpsMetadataInput input;
    VpsKeyMetadata key;
    TestInput source;
    int32_t explicit_key[1] = {3};
    size_t field;
    size_t bytes;
    (void)memset(&source, 0, sizeof(source));
    source.rows = 2U;
    source.fields = 16U;
    for (field = 0U; field < 16U; ++field) {
        source.values[0][field] = partial[field];
        source.values[1][field] = primary[field];
    }
    input.context = &source;
    input.row_count = 2U;
    input.field_count = 16U;
    input.is_null = test_input_is_null;
    input.value = test_input_value;
    input.length = test_input_length;
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   vps_metadata_rowset_init(&rows, &allocator, NULL) ==
                       VPS_METADATA_OK &&
                   vps_column_set_init(&columns, &allocator, NULL) ==
                       VPS_METADATA_OK &&
                   vps_size_multiply(3U, sizeof(VpsColumnMetadata), &bytes) ==
                       VPS_MEMORY_OK &&
                   vps_memory_allocate(&allocator, bytes,
                                       (void **)&columns.columns) ==
                       VPS_MEMORY_OK,
               "key_init");
    columns.columns_bytes = bytes;
    columns.column_count = 3U;
    columns.visible_count = 3U;
    (void)memset(columns.columns, 0, bytes);
    columns.columns[0].attribute_number = 1;
    columns.columns[0].not_null = 1;
    columns.columns[1].attribute_number = 2;
    columns.columns[1].domain_not_null = 1;
    columns.columns[2].attribute_number = 3;
    TEST_CHECK(vps_metadata_rowset_copy(&rows, VPS_CATALOG_QUERY_KEYS,
                                        &input) == VPS_METADATA_OK &&
                   vps_key_discover(&rows, &columns, NULL, 0U, 0, NULL,
                                    &key) == VPS_METADATA_OK &&
                   key.source == VPS_KEY_PRIMARY && key.index_oid == 50002U &&
                   key.column_count == 2U &&
                   key.attribute_numbers[0] == 1 &&
                   key.attribute_numbers[1] == 2 && !key.read_only,
               "key_primary_include_ignored");
    source.values[1][1] = "f";
    source.values[1][8] = "t";
    source.values[1][11] = "3";
    source.values[1][9] = "1";
    source.values[1][10] = "1";
    TEST_CHECK(vps_metadata_rowset_copy(&rows, VPS_CATALOG_QUERY_KEYS,
                                        &input) == VPS_METADATA_OK &&
                   vps_key_discover(&rows, &columns, NULL, 0U, 0, NULL,
                                    &key) == VPS_METADATA_OK &&
                   key.source == VPS_KEY_UNIQUE &&
                   key.nulls_not_distinct && key.attribute_numbers[0] == 3,
               "key_nulls_not_distinct");
    source.values[1][12] = "t";
    TEST_CHECK(vps_metadata_rowset_copy(&rows, VPS_CATALOG_QUERY_KEYS,
                                        &input) == VPS_METADATA_OK &&
                   vps_key_discover(&rows, &columns, NULL, 0U, 0, NULL,
                                    &key) == VPS_METADATA_OK &&
                   key.source == VPS_KEY_NONE && key.read_only,
               "key_deferrable_rejected");
    TEST_CHECK(vps_key_discover(&rows, &columns, explicit_key, 1U, 1, NULL,
                                &key) == VPS_METADATA_OK &&
                   key.source == VPS_KEY_EXPLICIT && !key.read_only,
               "key_explicit_validated");
    vps_column_set_reset(&columns);
    vps_metadata_rowset_reset(&rows);
    return 1;
}

static int test_relation_policy(void)
{
    static const char *partitioned[10] = {
        "41000", "p", "f", "t", "f", NULL, "r", "2", "1 2", "f"};
    VpsAllocator allocator;
    VpsMetadataRowSet rows;
    VpsRelationMetadata relation;
    VpsRelationPolicyMetadata policy;
    VpsKeyMetadata key;
    VpsMetadataInput input;
    TestInput source;
    size_t field;
    (void)memset(&source, 0, sizeof(source));
    source.rows = 1U;
    source.fields = 10U;
    for (field = 0U; field < 10U; ++field)
        source.values[0][field] = partitioned[field];
    input.context = &source;
    input.row_count = 1U;
    input.field_count = 10U;
    input.is_null = test_input_is_null;
    input.value = test_input_value;
    input.length = test_input_length;
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   vps_metadata_rowset_init(&rows, &allocator, NULL) ==
                       VPS_METADATA_OK &&
                   vps_relation_metadata_init(&relation, &allocator, NULL) ==
                       VPS_METADATA_OK,
               "policy_init");
    relation.relation_oid = 41000U;
    relation.kind = VPS_RELATION_PARTITIONED_TABLE;
    relation.writable_candidate = 1;
    relation.row_security = 1;
    (void)memset(&key, 0, sizeof(key));
    key.source = VPS_KEY_PRIMARY;
    key.column_count = 2U;
    key.attribute_numbers[0] = 1;
    key.attribute_numbers[1] = 2;
    TEST_CHECK(vps_metadata_rowset_copy(
                   &rows, VPS_CATALOG_QUERY_RELATION_POLICY, &input) ==
                       VPS_METADATA_OK &&
                   vps_relation_policy_build(&relation, &key, &rows, NULL,
                                             &policy) == VPS_METADATA_OK &&
                   policy.write_policy == VPS_RELATION_WRITE_ALLOWED &&
                   policy.partition_strategy == 'r' &&
                   policy.partition_attribute_count == 2U &&
                   policy.row_security && policy.zero_rows_ambiguous,
               "policy_partitioned");
    key.column_count = 1U;
    TEST_CHECK(vps_relation_policy_build(&relation, &key, &rows, NULL,
                                         &policy) == VPS_METADATA_OK &&
                   policy.write_policy ==
                       VPS_RELATION_WRITE_PARTITION_KEY_UNPROVEN,
               "policy_partition_key_missing");
    relation.kind = VPS_RELATION_INHERITANCE_PARENT;
    relation.row_security = 0;
    source.values[0][1] = "r";
    source.values[0][3] = "f";
    source.values[0][6] = NULL;
    source.values[0][7] = NULL;
    source.values[0][8] = NULL;
    key.column_count = 1U;
    TEST_CHECK(vps_metadata_rowset_copy(
                   &rows, VPS_CATALOG_QUERY_RELATION_POLICY, &input) ==
                       VPS_METADATA_OK &&
                   vps_relation_policy_build(&relation, &key, &rows, NULL,
                                             &policy) == VPS_METADATA_OK &&
                   policy.write_policy ==
                       VPS_RELATION_WRITE_INHERITANCE_UNSAFE,
               "policy_inheritance_read_only");
    key.source = VPS_KEY_NONE;
    key.read_only = 1;
    TEST_CHECK(vps_relation_policy_build(&relation, &key, &rows, NULL,
                                         &policy) == VPS_METADATA_OK &&
                   policy.write_policy == VPS_RELATION_WRITE_NO_KEY,
               "policy_no_key");
    vps_relation_metadata_reset(&relation);
    vps_metadata_rowset_reset(&rows);
    return 1;
}

static int test_fault_allocation(void)
{
    VpsAllocator system_allocator;
    VpsAllocator fault_allocator;
    VpsFaultAllocator fault;
    VpsMetadataRowSet rowset;
    VpsMetadataInput input;
    TestInput source;
    size_t field;
    (void)memset(&source, 0, sizeof(source));
    source.rows = 1U;
    source.fields = 12U;
    for (field = 0U; field < source.fields; ++field)
        source.values[0][field] = "1";
    input.context = &source;
    input.row_count = source.rows;
    input.field_count = source.fields;
    input.is_null = test_input_is_null;
    input.value = test_input_value;
    input.length = test_input_length;
    TEST_CHECK(vps_allocator_system(&system_allocator) == VPS_MEMORY_OK &&
                   vps_fault_allocator_init(&fault, &system_allocator, 1U) ==
                       VPS_MEMORY_OK &&
                   vps_fault_allocator_make(&fault, &fault_allocator) ==
                       VPS_MEMORY_OK &&
                   vps_metadata_rowset_init(&rowset, &fault_allocator, NULL) ==
                       VPS_METADATA_OK,
               "fault_init");
    TEST_CHECK(vps_metadata_rowset_copy(&rowset,
                                        VPS_CATALOG_QUERY_RELATION, &input) ==
                       VPS_METADATA_OUT_OF_MEMORY &&
                   rowset.row_count == 0U && fault.active_allocations == 0U,
               "fault_cells");
    TEST_CHECK(vps_fault_allocator_reset(&fault, 2U) == VPS_MEMORY_OK &&
                   vps_metadata_rowset_copy(
                       &rowset, VPS_CATALOG_QUERY_RELATION, &input) ==
                       VPS_METADATA_OUT_OF_MEMORY &&
                   rowset.row_count == 0U && fault.active_allocations == 0U,
               "fault_bytes");
    vps_metadata_rowset_reset(&rowset);
    TEST_CHECK(fault.active_allocations == 0U && fault.cleanup_errors == 0U,
               "fault_cleanup");
    return 1;
}

int main(void)
{
    if (!test_query_specs() || !test_rowset_copy_and_atomicity() ||
        !test_statement_contract() || !test_relation_resolution()) return 1;
    if (!test_column_metadata()) return 1;
    if (!test_key_discovery()) return 1;
    if (!test_relation_policy() || !test_fault_allocation()) return 1;
    (void)printf("metadata_tests=passed\n");
    return 0;
}
