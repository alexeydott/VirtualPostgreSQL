#ifndef VPS_QUERY_SOURCE_H
#define VPS_QUERY_SOURCE_H

#include "vps_logging.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_QUERY_SOURCE_FORMAT_VERSION UINT32_C(1)
#define VPS_QUERY_SOURCE_MAX_BYTES (1024U * 1024U)
#define VPS_QUERY_SOURCE_MAX_NESTING 256U
#define VPS_QUERY_SOURCE_MAX_TOKENS 262144U

typedef enum VpsQuerySourceResult {
    VPS_QUERY_SOURCE_OK = 0,
    VPS_QUERY_SOURCE_INVALID_ARGUMENT = 1,
    VPS_QUERY_SOURCE_LIMIT_EXCEEDED = 2,
    VPS_QUERY_SOURCE_INVALID_UTF8 = 3,
    VPS_QUERY_SOURCE_CONTAINS_NUL = 4,
    VPS_QUERY_SOURCE_UNTERMINATED = 5,
    VPS_QUERY_SOURCE_MULTIPLE_STATEMENTS = 6,
    VPS_QUERY_SOURCE_UNSUPPORTED_ROOT = 7,
    VPS_QUERY_SOURCE_FORBIDDEN_COMMAND = 8,
    VPS_QUERY_SOURCE_DATA_MODIFYING_CTE = 9,
    VPS_QUERY_SOURCE_UNRESOLVED_PARAMETER = 10,
    VPS_QUERY_SOURCE_LOCKING_SELECT = 11,
    VPS_QUERY_SOURCE_SELECT_INTO = 12,
    VPS_QUERY_SOURCE_UNBALANCED = 13
} VpsQuerySourceResult;

typedef enum VpsQuerySourceRoot {
    VPS_QUERY_ROOT_NONE = 0,
    VPS_QUERY_ROOT_SELECT = 1,
    VPS_QUERY_ROOT_WITH_SELECT = 2
} VpsQuerySourceRoot;

typedef struct VpsQuerySourceAnalysis {
    uint32_t format_version;
    VpsQuerySourceRoot root;
    VpsQuerySourceResult result;
    size_t error_offset;
    size_t token_count;
    size_t maximum_nesting;
    size_t statement_length;
    uint64_t normalized_hash;
    int has_terminal_semicolon;
} VpsQuerySourceAnalysis;

/*
 * query is a borrowed byte span and need not be NUL-terminated. The scanner
 * allocates nothing and never retains or logs query bytes. analysis is always
 * initialized when non-NULL. It is safe for concurrent callers when logger
 * emission is externally serialized as required by VpsLogger.
 */
VpsQuerySourceResult vps_query_source_scan(
    const char *query,
    size_t query_length,
    VpsLogger *logger,
    VpsQuerySourceAnalysis *analysis);

const char *vps_query_source_result_name(VpsQuerySourceResult result);
const char *vps_query_source_root_name(VpsQuerySourceRoot root);

#endif
