#ifndef VPS_ROW_IDENTITY_H
#define VPS_ROW_IDENTITY_H

#include "vps_client.h"
#include "vps_memory.h"

#include <stdint.h>

#define VPS_ROW_IDENTITY_FORMAT_VERSION 1U
#define VPS_ROW_IDENTITY_MAX_BYTES 65536U
#define VPS_ROW_IDENTITY_MAX_FIELDS 32U

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

typedef enum VpsRowIdentityFieldKind {
    VPS_ROW_IDENTITY_FIELD_NULL = 0,
    VPS_ROW_IDENTITY_FIELD_INTEGER = 1,
    VPS_ROW_IDENTITY_FIELD_TEXT = 2,
    VPS_ROW_IDENTITY_FIELD_BLOB = 3
} VpsRowIdentityFieldKind;

typedef struct VpsRowIdentityField {
    VpsRowIdentityFieldKind kind;
    uint32_t type_oid;
    const void *bytes;
    size_t length;
    int64_t integer;
} VpsRowIdentityField;

typedef struct VpsRowIdentitySpec {
    uint32_t relation_oid;
    const VpsRowIdentityField *key_fields;
    size_t key_field_count;
    const VpsRowIdentityField *optimistic_field;
} VpsRowIdentitySpec;

/* Decoded fields borrow token bytes and remain valid only while token lives. */
typedef struct VpsRowIdentityView {
    uint32_t relation_oid;
    VpsRowIdentityField key_fields[VPS_ROW_IDENTITY_MAX_FIELDS];
    size_t key_field_count;
    VpsRowIdentityField optimistic_field;
    int has_optimistic_field;
} VpsRowIdentityView;

VpsRowIdentityResult vps_row_identity_stable_integer(
    const VpsClientColumnView *column, int64_t *rowid);
VpsRowIdentityResult vps_row_identity_token(
    const VpsAllocator *allocator,
    const VpsClientColumnView *columns,
    size_t column_count,
    VpsBuffer *token);
VpsRowIdentityResult vps_row_identity_encode(
    const VpsAllocator *allocator,
    const VpsRowIdentitySpec *spec,
    VpsBuffer *token);
VpsRowIdentityResult vps_row_identity_decode(
    const void *token,
    size_t token_length,
    VpsRowIdentityView *view);
VpsRowIdentityResult vps_row_identity_scan_next(uint64_t *counter,
                                                int64_t *rowid);
const char *vps_row_identity_mode_name(VpsRowIdentityMode mode);

#endif
