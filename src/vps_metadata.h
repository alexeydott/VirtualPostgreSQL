#ifndef VPS_METADATA_H
#define VPS_METADATA_H

#include "vps_logging.h"
#include "vps_memory.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_METADATA_FORMAT_VERSION UINT32_C(1)
#define VPS_METADATA_MAX_ROWS 65536U
#define VPS_METADATA_MAX_FIELDS 64U
#define VPS_METADATA_MAX_CELLS (VPS_METADATA_MAX_ROWS * VPS_METADATA_MAX_FIELDS)
#define VPS_METADATA_MAX_CELL_BYTES (1024U * 1024U)
#define VPS_METADATA_MAX_TOTAL_BYTES (16U * 1024U * 1024U)
#define VPS_METADATA_NAME_MAX_BYTES 255U
#define VPS_METADATA_MAX_KEY_COLUMNS 32U
#define VPS_METADATA_TEXT_OID UINT32_C(25)
#define VPS_METADATA_NAME_OID UINT32_C(19)

typedef enum VpsMetadataResult {
    VPS_METADATA_OK = 0,
    VPS_METADATA_INVALID_ARGUMENT = 1,
    VPS_METADATA_INVALID_STATE = 2,
    VPS_METADATA_INVALID_RESULT = 3,
    VPS_METADATA_LIMIT_EXCEEDED = 4,
    VPS_METADATA_OUT_OF_MEMORY = 5,
    VPS_METADATA_NOT_FOUND = 6,
    VPS_METADATA_UNSUPPORTED = 7
} VpsMetadataResult;

typedef enum VpsCatalogQuery {
    VPS_CATALOG_QUERY_RELATION = 0,
    VPS_CATALOG_QUERY_COLUMNS = 1,
    VPS_CATALOG_QUERY_KEYS = 2,
    VPS_CATALOG_QUERY_RELATION_POLICY = 3,
    VPS_CATALOG_QUERY_POSTGIS = 4,
    VPS_CATALOG_QUERY_COUNT = 5
} VpsCatalogQuery;

typedef enum VpsRelationKind {
    VPS_RELATION_TABLE = 0,
    VPS_RELATION_PARTITIONED_TABLE = 1,
    VPS_RELATION_VIEW = 2,
    VPS_RELATION_MATERIALIZED_VIEW = 3,
    VPS_RELATION_FOREIGN_TABLE = 4,
    VPS_RELATION_INHERITANCE_PARENT = 5,
    VPS_RELATION_PARTITION = 6,
    VPS_RELATION_SEQUENCE = 7,
    VPS_RELATION_COMPOSITE = 8,
    VPS_RELATION_TOAST = 9,
    VPS_RELATION_INDEX = 10,
    VPS_RELATION_UNSUPPORTED = 11
} VpsRelationKind;

typedef struct VpsCatalogQuerySpec {
    VpsCatalogQuery query;
    const char *sql;
    size_t sql_length;
    size_t parameter_count;
    size_t result_field_count;
} VpsCatalogQuerySpec;

typedef struct VpsMetadataCell {
    size_t offset;
    size_t length;
    int is_null;
} VpsMetadataCell;

/*
 * RowSet owns cells and copied bytes through its allocator. Cell pointers are
 * views into bytes and remain valid until reset. Initialization and reset are
 * idempotent; mutation is caller-serialized and publish happens only after a
 * complete successful copy.
 */
typedef struct VpsMetadataRowSet {
    VpsAllocator allocator;
    VpsMetadataCell *cells;
    unsigned char *bytes;
    size_t row_count;
    size_t field_count;
    size_t cell_count;
    size_t cell_bytes;
    size_t bytes_size;
    VpsLogger *logger;
    int initialized;
} VpsMetadataRowSet;

typedef struct VpsMetadataInput {
    void *context;
    size_t row_count;
    size_t field_count;
    int (*is_null)(void *context, size_t row, size_t field);
    const void *(*value)(void *context, size_t row, size_t field);
    size_t (*length)(void *context, size_t row, size_t field);
} VpsMetadataInput;

typedef struct VpsRelationMetadata {
    VpsAllocator allocator;
    VpsBuffer schema_name;
    VpsBuffer relation_name;
    uint32_t namespace_oid;
    uint32_t relation_oid;
    uint32_t access_method_oid;
    VpsRelationKind kind;
    char persistence;
    int is_partition;
    int has_children;
    int has_parent;
    int row_security;
    int force_row_security;
    int readable;
    int writable_candidate;
    VpsLogger *logger;
    int initialized;
} VpsRelationMetadata;

typedef struct VpsMetadataString {
    size_t offset;
    size_t length;
    int present;
} VpsMetadataString;

typedef struct VpsColumnMetadata {
    int32_t attribute_number;
    uint32_t type_oid;
    int32_t type_modifier;
    uint32_t type_namespace_oid;
    uint32_t domain_base_oid;
    int32_t domain_base_modifier;
    uint32_t array_element_oid;
    uint32_t collation_oid;
    uint32_t origin_relation_oid;
    int32_t origin_attribute_number;
    int32_t statistics_target;
    VpsMetadataString name;
    VpsMetadataString type_namespace;
    VpsMetadataString type_name;
    VpsMetadataString domain_base_namespace;
    VpsMetadataString domain_base_name;
    VpsMetadataString collation_name;
    VpsMetadataString default_expression_hash;
    VpsMetadataString domain_default_hash;
    VpsMetadataString domain_constraint_hash;
    VpsMetadataString formatted_type;
    char type_category;
    char type_kind;
    char domain_base_category;
    char domain_base_kind;
    char generated_kind;
    char identity_kind;
    char storage_kind;
    char compression_kind;
    char collation_provider;
    char default_kind;
    int not_null;
    int dropped;
    int has_default;
    int domain_not_null;
    int domain_has_default;
    int collation_deterministic;
    int has_comment;
} VpsColumnMetadata;

typedef struct VpsColumnSet {
    VpsAllocator allocator;
    VpsColumnMetadata *columns;
    size_t column_count;
    size_t visible_count;
    size_t columns_bytes;
    VpsBuffer text;
    VpsLogger *logger;
    int initialized;
} VpsColumnSet;

typedef enum VpsKeySource {
    VPS_KEY_NONE = 0,
    VPS_KEY_PRIMARY = 1,
    VPS_KEY_UNIQUE = 2,
    VPS_KEY_EXPLICIT = 3
} VpsKeySource;

typedef struct VpsKeyMetadata {
    VpsKeySource source;
    uint32_t index_oid;
    int32_t attribute_numbers[VPS_METADATA_MAX_KEY_COLUMNS];
    size_t column_count;
    int nulls_not_distinct;
    int read_only;
} VpsKeyMetadata;

typedef enum VpsRelationWritePolicy {
    VPS_RELATION_WRITE_ALLOWED = 0,
    VPS_RELATION_WRITE_NO_KEY = 1,
    VPS_RELATION_WRITE_SOURCE_READ_ONLY = 2,
    VPS_RELATION_WRITE_INHERITANCE_UNSAFE = 3,
    VPS_RELATION_WRITE_PARTITION_KEY_UNPROVEN = 4
} VpsRelationWritePolicy;

typedef struct VpsRelationPolicyMetadata {
    uint32_t parent_oids[VPS_METADATA_MAX_KEY_COLUMNS];
    int32_t partition_attribute_numbers[VPS_METADATA_MAX_KEY_COLUMNS];
    size_t parent_count;
    size_t partition_attribute_count;
    char partition_strategy;
    int row_security;
    int force_row_security;
    int zero_rows_ambiguous;
    VpsRelationWritePolicy write_policy;
} VpsRelationPolicyMetadata;

VpsMetadataResult vps_metadata_catalog_query_spec(
    VpsCatalogQuery query,
    VpsCatalogQuerySpec *spec);
VpsMetadataResult vps_metadata_rowset_init(VpsMetadataRowSet *rowset,
                                           const VpsAllocator *allocator,
                                           VpsLogger *logger);
VpsMetadataResult vps_metadata_rowset_copy(VpsMetadataRowSet *rowset,
                                           VpsCatalogQuery query,
                                           const VpsMetadataInput *input);
VpsMetadataResult vps_metadata_rowset_append(VpsMetadataRowSet *rowset,
                                             VpsCatalogQuery query,
                                             const VpsMetadataInput *input);
VpsMetadataResult vps_metadata_rowset_cell(
    const VpsMetadataRowSet *rowset,
    size_t row,
    size_t field,
    const unsigned char **value,
    size_t *length,
    int *is_null);
void vps_metadata_rowset_reset(VpsMetadataRowSet *rowset);
VpsMetadataResult vps_relation_metadata_init(
    VpsRelationMetadata *relation,
    const VpsAllocator *allocator,
    VpsLogger *logger);
VpsMetadataResult vps_relation_metadata_resolve(
    VpsRelationMetadata *relation,
    const VpsMetadataRowSet *rowset,
    const char *expected_schema,
    size_t expected_schema_length,
    const char *expected_relation,
    size_t expected_relation_length);
void vps_relation_metadata_reset(VpsRelationMetadata *relation);
VpsMetadataResult vps_column_set_init(VpsColumnSet *columns,
                                      const VpsAllocator *allocator,
                                      VpsLogger *logger);
VpsMetadataResult vps_column_set_build(VpsColumnSet *columns,
                                       const VpsMetadataRowSet *rowset);
VpsMetadataResult vps_column_set_string(
    const VpsColumnSet *columns,
    const VpsMetadataString *string,
    const unsigned char **value,
    size_t *length);
void vps_column_set_reset(VpsColumnSet *columns);
VpsMetadataResult vps_key_discover(
    const VpsMetadataRowSet *index_rows,
    const VpsColumnSet *columns,
    const int32_t *explicit_attribute_numbers,
    size_t explicit_count,
    int explicit_validated,
    VpsLogger *logger,
    VpsKeyMetadata *key);
const char *vps_key_source_name(VpsKeySource source);
VpsMetadataResult vps_relation_policy_build(
    const VpsRelationMetadata *relation,
    const VpsKeyMetadata *key,
    const VpsMetadataRowSet *policy_rows,
    VpsLogger *logger,
    VpsRelationPolicyMetadata *policy);
const char *vps_relation_write_policy_name(VpsRelationWritePolicy policy);
const char *vps_metadata_result_name(VpsMetadataResult result);
const char *vps_catalog_query_name(VpsCatalogQuery query);
const char *vps_relation_kind_name(VpsRelationKind kind);

#endif
