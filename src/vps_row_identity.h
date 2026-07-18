#ifndef VPS_ROW_IDENTITY_H
#define VPS_ROW_IDENTITY_H

#include "vps_client.h"
#include "vps_memory.h"

#include <stdint.h>

#define VPS_ROW_IDENTITY_FORMAT_VERSION 1U
#define VPS_ROW_IDENTITY_MAX_BYTES 65536U

typedef enum VpsRowIdentityMode {
    VPS_ROW_IDENTITY_STABLE_INTEGER = 0,
    VPS_ROW_IDENTITY_HIDDEN_TOKEN = 1,
    VPS_ROW_IDENTITY_SCAN_LOCAL = 2
} VpsRowIdentityMode;

typedef enum VpsRowIdentityResult {
    VPS_ROW_IDENTITY_OK = 0,
    VPS_ROW_IDENTITY_INVALID_ARGUMENT = 1,
    VPS_ROW_IDENTITY_MALFORMED = 2,
    VPS_ROW_IDENTITY_LIMIT = 3,
    VPS_ROW_IDENTITY_OUT_OF_MEMORY = 4
} VpsRowIdentityResult;

VpsRowIdentityResult vps_row_identity_stable_integer(
    const VpsClientColumnView *column, int64_t *rowid);
VpsRowIdentityResult vps_row_identity_token(
    const VpsAllocator *allocator,
    const VpsClientColumnView *columns,
    size_t column_count,
    VpsBuffer *token);
VpsRowIdentityResult vps_row_identity_scan_next(uint64_t *counter,
                                                int64_t *rowid);
const char *vps_row_identity_mode_name(VpsRowIdentityMode mode);

#endif
