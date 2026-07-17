#ifndef VPS_ERROR_H
#define VPS_ERROR_H

#include "vps_memory.h"

#include <stddef.h>

#define VPS_SQLSTATE_LENGTH 5U
#define VPS_SQLSTATE_BUFFER_SIZE 6U
#define VPS_ERROR_FINGERPRINT_LENGTH 64U
#define VPS_ERROR_FINGERPRINT_BUFFER_SIZE 65U
#define VPS_ERROR_MESSAGE_LIMIT 255U

typedef enum VpsErrorClass {
    VPS_ERROR_CLASS_NONE = 0,
    VPS_ERROR_CLASS_CONFIG = 1,
    VPS_ERROR_CLASS_AUTH = 2,
    VPS_ERROR_CLASS_TLS = 3,
    VPS_ERROR_CLASS_CONNECTION = 4,
    VPS_ERROR_CLASS_TIMEOUT = 5,
    VPS_ERROR_CLASS_CANCEL = 6,
    VPS_ERROR_CLASS_POOL = 7,
    VPS_ERROR_CLASS_SQL = 8,
    VPS_ERROR_CLASS_QUERY_SOURCE = 9,
    VPS_ERROR_CLASS_METADATA = 10,
    VPS_ERROR_CLASS_SCHEMA = 11,
    VPS_ERROR_CLASS_CONSTRAINT = 12,
    VPS_ERROR_CLASS_SERIALIZATION = 13,
    VPS_ERROR_CLASS_DEADLOCK = 14,
    VPS_ERROR_CLASS_LOCK = 15,
    VPS_ERROR_CLASS_CONVERSION = 16,
    VPS_ERROR_CLASS_SPATIAL = 17,
    VPS_ERROR_CLASS_MEMORY = 18,
    VPS_ERROR_CLASS_TRANSACTION = 19,
    VPS_ERROR_CLASS_INVARIANT = 20,
    VPS_ERROR_CLASS_UNSUPPORTED = 21
} VpsErrorClass;

typedef enum VpsErrorOperation {
    VPS_ERROR_OPERATION_NONE = 0,
    VPS_ERROR_OPERATION_CONFIGURE = 1,
    VPS_ERROR_OPERATION_CONNECT = 2,
    VPS_ERROR_OPERATION_POOL_WAIT = 3,
    VPS_ERROR_OPERATION_METADATA = 4,
    VPS_ERROR_OPERATION_SCAN = 5,
    VPS_ERROR_OPERATION_QUERY = 6,
    VPS_ERROR_OPERATION_DML = 7,
    VPS_ERROR_OPERATION_CANCEL = 8,
    VPS_ERROR_OPERATION_COMMIT = 9,
    VPS_ERROR_OPERATION_ROLLBACK = 10
} VpsErrorOperation;

/*
 * VpsError owns its canonical, redaction-safe message through message_storage.
 * init selects the allocator for the lifetime of the value. reset is
 * idempotent and preserves that allocator. copy and setters replace state only
 * after all required allocation succeeds. The value is not thread-safe.
 *
 * Raw backend DETAIL, HINT, CONTEXT, query text and values have no input fields
 * in this contract and therefore cannot be retained accidentally.
 */
typedef struct VpsError {
    VpsOwnedMemory message_storage;
    int sqlite_code;
    char sqlstate[VPS_SQLSTATE_BUFFER_SIZE];
    int backend_status;
    int severity_class;
    VpsErrorOperation operation;
    char query_fingerprint[VPS_ERROR_FINGERPRINT_BUFFER_SIZE];
    VpsErrorClass error_class;
    int transient;
    int ambiguous;
    int initialized;
} VpsError;

VpsMemoryResult vps_error_init(VpsError *error,
                               const VpsAllocator *allocator);
void vps_error_reset(VpsError *error);

VpsMemoryResult vps_error_set_sqlstate(
    VpsError *error,
    VpsErrorOperation operation,
    const char *sqlstate,
    int backend_status,
    int severity_class,
    const char *query_fingerprint);
VpsMemoryResult vps_error_set_local(VpsError *error,
                                    VpsErrorOperation operation,
                                    VpsErrorClass error_class,
                                    const char *query_fingerprint);
VpsMemoryResult vps_error_copy(VpsError *destination,
                               const VpsError *source);

const char *vps_error_message(const VpsError *error);
const char *vps_error_class_name(VpsErrorClass error_class);
const char *vps_error_operation_name(VpsErrorOperation operation);

#endif
