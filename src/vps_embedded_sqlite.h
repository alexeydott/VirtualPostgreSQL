#ifndef VPS_EMBEDDED_SQLITE_H
#define VPS_EMBEDDED_SQLITE_H

#include "vps_logging.h"
#include "vps_memory.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_EMBEDDED_SQLITE_CONTRACT_VERSION UINT32_C(1)
#define VPS_EMBEDDED_MAX_COLUMNS 1024U
#define VPS_EMBEDDED_MAX_INDEXES 64U
#define VPS_EMBEDDED_MAX_INDEX_COLUMNS 32U
#define VPS_EMBEDDED_MAX_CONSTRAINTS 64U
#define VPS_EMBEDDED_MAX_ORDER_TERMS 32U

typedef enum VpsEmbeddedSqliteStatus {
    VPS_EMBEDDED_SQLITE_OK = 0,
    VPS_EMBEDDED_SQLITE_INVALID_ARGUMENT = 1,
    VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY = 2,
    VPS_EMBEDDED_SQLITE_LIMIT_EXCEEDED = 3,
    VPS_EMBEDDED_SQLITE_BUSY = 4,
    VPS_EMBEDDED_SQLITE_IO_ERROR = 5,
    VPS_EMBEDDED_SQLITE_ERROR = 6
} VpsEmbeddedSqliteStatus;

typedef enum VpsEmbeddedSqliteMode {
    VPS_EMBEDDED_SQLITE_MEMORY = 1,
    VPS_EMBEDDED_SQLITE_TEMP = 2
} VpsEmbeddedSqliteMode;

typedef struct VpsEmbeddedSqlite VpsEmbeddedSqlite;
typedef struct VpsEmbeddedSqliteScan VpsEmbeddedSqliteScan;

typedef enum VpsEmbeddedValueKind {
    VPS_EMBEDDED_VALUE_NULL = 0,
    VPS_EMBEDDED_VALUE_INTEGER = 1,
    VPS_EMBEDDED_VALUE_REAL = 2,
    VPS_EMBEDDED_VALUE_TEXT = 3,
    VPS_EMBEDDED_VALUE_BLOB = 4
} VpsEmbeddedValueKind;

typedef struct VpsEmbeddedValue {
    VpsEmbeddedValueKind kind;
    const void *bytes;
    size_t length;
    int64_t integer;
    double real;
} VpsEmbeddedValue;

typedef struct VpsEmbeddedIndexDefinition {
    uint16_t columns[VPS_EMBEDDED_MAX_INDEX_COLUMNS];
    size_t column_count;
    uint64_t name_hash;
} VpsEmbeddedIndexDefinition;

typedef struct VpsEmbeddedSchema {
    const VpsEmbeddedValueKind *column_kinds;
    size_t column_count;
    const VpsEmbeddedIndexDefinition *indexes;
    size_t index_count;
    uint64_t source_fingerprint;
    uint64_t layout_fingerprint;
} VpsEmbeddedSchema;

typedef enum VpsEmbeddedOperator {
    VPS_EMBEDDED_OP_EQ = 1,
    VPS_EMBEDDED_OP_NE = 2,
    VPS_EMBEDDED_OP_LT = 3,
    VPS_EMBEDDED_OP_LE = 4,
    VPS_EMBEDDED_OP_GT = 5,
    VPS_EMBEDDED_OP_GE = 6,
    VPS_EMBEDDED_OP_IS_NULL = 7,
    VPS_EMBEDDED_OP_IS_NOT_NULL = 8
} VpsEmbeddedOperator;

typedef struct VpsEmbeddedConstraint {
    uint16_t column;
    VpsEmbeddedOperator operation;
    VpsEmbeddedValue value;
} VpsEmbeddedConstraint;

typedef struct VpsEmbeddedOrderTerm {
    uint16_t column;
    int descending;
} VpsEmbeddedOrderTerm;

typedef struct VpsEmbeddedScanRequest {
    const uint16_t *projection;
    size_t projection_count;
    const VpsEmbeddedConstraint *constraints;
    size_t constraint_count;
    const VpsEmbeddedOrderTerm *order_terms;
    size_t order_count;
    size_t selected_index;
    int use_index;
    uint64_t limit;
    uint64_t offset;
    int has_limit;
    int has_offset;
} VpsEmbeddedScanRequest;

typedef struct VpsEmbeddedSqliteOpenOptions {
    VpsAllocator allocator;
    VpsLogger *logger;
    VpsEmbeddedSqliteMode mode;
    /* UTF-8 private path for TEMP. Borrowed only for the duration of open. */
    const char *temp_path;
    size_t temp_path_length;
} VpsEmbeddedSqliteOpenOptions;

/*
 * The returned handle is owned by the caller and exposes no sqlite3 type.
 * Host sqlite3 handles and host-allocator memory are not valid inputs.
 * close is idempotent and deletes an owned TEMP path on a best-effort basis.
 */
VpsEmbeddedSqliteStatus vps_embedded_sqlite_open(
    const VpsEmbeddedSqliteOpenOptions *options,
    VpsEmbeddedSqlite **database);
VpsEmbeddedSqliteStatus vps_embedded_sqlite_close(
    VpsEmbeddedSqlite **database);

VpsEmbeddedSqliteStatus vps_embedded_sqlite_create_schema(
    VpsEmbeddedSqlite *database,
    const VpsEmbeddedSchema *schema);
VpsEmbeddedSqliteStatus vps_embedded_sqlite_append_row(
    VpsEmbeddedSqlite *database,
    const VpsEmbeddedValue *values,
    size_t value_count);
VpsEmbeddedSqliteStatus vps_embedded_sqlite_seal(
    VpsEmbeddedSqlite *database);
VpsEmbeddedSqliteStatus vps_embedded_sqlite_abort(
    VpsEmbeddedSqlite *database);

VpsEmbeddedSqliteStatus vps_embedded_sqlite_scan_open(
    VpsEmbeddedSqlite *database,
    const VpsEmbeddedScanRequest *request,
    VpsEmbeddedSqliteScan **scan);
VpsEmbeddedSqliteStatus vps_embedded_sqlite_scan_step(
    VpsEmbeddedSqliteScan *scan,
    int *has_row);
VpsEmbeddedSqliteStatus vps_embedded_sqlite_scan_column(
    const VpsEmbeddedSqliteScan *scan,
    size_t column,
    VpsEmbeddedValue *value);
VpsEmbeddedSqliteStatus vps_embedded_sqlite_scan_close(
    VpsEmbeddedSqliteScan **scan);
int vps_embedded_sqlite_scan_uses_index(
    const VpsEmbeddedSqliteScan *scan);

uint64_t vps_embedded_sqlite_row_count(const VpsEmbeddedSqlite *database);
uint64_t vps_embedded_sqlite_byte_count(const VpsEmbeddedSqlite *database);
uint64_t vps_embedded_sqlite_source_fingerprint(
    const VpsEmbeddedSqlite *database);
uint64_t vps_embedded_sqlite_layout_fingerprint(
    const VpsEmbeddedSqlite *database);

int vps_embedded_sqlite_version_number(void);
const char *vps_embedded_sqlite_version(void);
uint64_t vps_embedded_sqlite_compile_options_fingerprint(void);
const char *vps_embedded_sqlite_status_name(VpsEmbeddedSqliteStatus status);

#endif
