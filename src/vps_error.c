#include "vps_error.h"

#include <sqlite3.h>

#include <string.h>

typedef struct VpsErrorMapping {
    int sqlite_code;
    VpsErrorClass error_class;
    int transient;
    int ambiguous;
    const char *message;
} VpsErrorMapping;

static int vps_error_operation_is_valid(VpsErrorOperation operation)
{
    return operation >= VPS_ERROR_OPERATION_CONFIGURE &&
           operation <= VPS_ERROR_OPERATION_ROLLBACK;
}

static int vps_error_class_is_valid(VpsErrorClass error_class)
{
    return error_class >= VPS_ERROR_CLASS_CONFIG &&
           error_class <= VPS_ERROR_CLASS_UNSUPPORTED;
}

static int vps_error_sqlstate_is_valid(const char *sqlstate)
{
    size_t index;

    if (sqlstate == NULL) {
        return 0;
    }
    for (index = 0U; index < VPS_SQLSTATE_LENGTH; ++index) {
        char value = sqlstate[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'Z'))) {
            return 0;
        }
    }
    return sqlstate[VPS_SQLSTATE_LENGTH] == '\0';
}

static int vps_error_fingerprint_is_valid(const char *fingerprint)
{
    size_t index;

    if (fingerprint == NULL || fingerprint[0] == '\0') {
        return 1;
    }
    for (index = 0U; index < VPS_ERROR_FINGERPRINT_LENGTH; ++index) {
        char value = fingerprint[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return 0;
        }
    }
    return fingerprint[VPS_ERROR_FINGERPRINT_LENGTH] == '\0';
}

static VpsMemoryResult vps_error_bounded_message_size(const char *message,
                                                      size_t *message_size)
{
    size_t index;

    if (message == NULL || message_size == NULL) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    for (index = 0U; index <= VPS_ERROR_MESSAGE_LIMIT; ++index) {
        if (message[index] == '\0') {
            *message_size = index;
            return VPS_MEMORY_OK;
        }
    }
    return VPS_MEMORY_LIMIT_EXCEEDED;
}

static int vps_error_is_schema_operation(VpsErrorOperation operation)
{
    return operation == VPS_ERROR_OPERATION_METADATA ||
           operation == VPS_ERROR_OPERATION_SCAN;
}

static VpsErrorMapping vps_error_map_sqlstate(
    VpsErrorOperation operation,
    const char *sqlstate)
{
    VpsErrorMapping mapping = {SQLITE_ERROR, VPS_ERROR_CLASS_SQL, 0, 0,
                               "PostgreSQL operation failed"};

    if (strcmp(sqlstate, "23505") == 0) {
        mapping.sqlite_code = SQLITE_CONSTRAINT_UNIQUE;
        mapping.error_class = VPS_ERROR_CLASS_CONSTRAINT;
        mapping.message = "unique constraint violation";
    } else if (strcmp(sqlstate, "23503") == 0) {
        mapping.sqlite_code = SQLITE_CONSTRAINT_FOREIGNKEY;
        mapping.error_class = VPS_ERROR_CLASS_CONSTRAINT;
        mapping.message = "foreign key constraint violation";
    } else if (strcmp(sqlstate, "23502") == 0) {
        mapping.sqlite_code = SQLITE_CONSTRAINT_NOTNULL;
        mapping.error_class = VPS_ERROR_CLASS_CONSTRAINT;
        mapping.message = "not-null constraint violation";
    } else if (strcmp(sqlstate, "23514") == 0) {
        mapping.sqlite_code = SQLITE_CONSTRAINT_CHECK;
        mapping.error_class = VPS_ERROR_CLASS_CONSTRAINT;
        mapping.message = "check constraint violation";
    } else if (strcmp(sqlstate, "40001") == 0) {
        mapping.sqlite_code = SQLITE_BUSY;
        mapping.error_class = VPS_ERROR_CLASS_SERIALIZATION;
        mapping.transient = 1;
        mapping.message = "serialization failure";
    } else if (strcmp(sqlstate, "40P01") == 0) {
        mapping.sqlite_code = SQLITE_BUSY;
        mapping.error_class = VPS_ERROR_CLASS_DEADLOCK;
        mapping.transient = 1;
        mapping.message = "deadlock detected";
    } else if (strcmp(sqlstate, "55P03") == 0) {
        mapping.sqlite_code = SQLITE_BUSY;
        mapping.error_class = VPS_ERROR_CLASS_LOCK;
        mapping.transient = 1;
        mapping.message = "lock not available";
    } else if (strcmp(sqlstate, "57014") == 0) {
        mapping.sqlite_code = SQLITE_INTERRUPT;
        mapping.error_class = VPS_ERROR_CLASS_CANCEL;
        mapping.message = "operation canceled";
    } else if (strcmp(sqlstate, "28P01") == 0) {
        mapping.sqlite_code = SQLITE_AUTH;
        mapping.error_class = VPS_ERROR_CLASS_AUTH;
        mapping.message = "authentication failed";
    } else if (strcmp(sqlstate, "42501") == 0) {
        mapping.sqlite_code = operation == VPS_ERROR_OPERATION_CONNECT
                                  ? SQLITE_AUTH
                                  : SQLITE_PERM;
        mapping.error_class = VPS_ERROR_CLASS_AUTH;
        mapping.message = "insufficient privilege";
    } else if ((strcmp(sqlstate, "42P01") == 0 ||
                strcmp(sqlstate, "42703") == 0) &&
               vps_error_is_schema_operation(operation)) {
        mapping.sqlite_code = SQLITE_SCHEMA;
        mapping.error_class = VPS_ERROR_CLASS_SCHEMA;
        mapping.message = "remote schema changed";
    } else if (sqlstate[0] == '0' && sqlstate[1] == '8') {
        mapping.sqlite_code = operation == VPS_ERROR_OPERATION_CONNECT
                                  ? SQLITE_CANTOPEN
                                  : SQLITE_IOERR;
        mapping.error_class = VPS_ERROR_CLASS_CONNECTION;
        mapping.transient = 1;
        mapping.ambiguous = operation == VPS_ERROR_OPERATION_COMMIT;
        mapping.message = mapping.ambiguous
                              ? "commit outcome is unknown"
                              : "PostgreSQL connection failed";
    }
    return mapping;
}

static VpsErrorMapping vps_error_map_local(VpsErrorOperation operation,
                                           VpsErrorClass error_class)
{
    VpsErrorMapping mapping = {SQLITE_ERROR, error_class, 0, 0,
                               "extension operation failed"};

    switch (error_class) {
    case VPS_ERROR_CLASS_CONFIG:
        mapping.message = "invalid extension configuration";
        break;
    case VPS_ERROR_CLASS_AUTH:
        mapping.sqlite_code = SQLITE_AUTH;
        mapping.message = "authentication failed";
        break;
    case VPS_ERROR_CLASS_TLS:
        mapping.sqlite_code = SQLITE_AUTH;
        mapping.message = "TLS validation failed";
        break;
    case VPS_ERROR_CLASS_CONNECTION:
        mapping.sqlite_code = operation == VPS_ERROR_OPERATION_CONNECT
                                  ? SQLITE_CANTOPEN
                                  : SQLITE_IOERR;
        mapping.transient = 1;
        mapping.ambiguous = operation == VPS_ERROR_OPERATION_COMMIT;
        mapping.message = mapping.ambiguous
                              ? "commit outcome is unknown"
                              : "PostgreSQL connection failed";
        break;
    case VPS_ERROR_CLASS_TIMEOUT:
        mapping.sqlite_code = operation == VPS_ERROR_OPERATION_POOL_WAIT
                                  ? SQLITE_BUSY
                                  : SQLITE_INTERRUPT;
        mapping.message = "operation timed out";
        break;
    case VPS_ERROR_CLASS_CANCEL:
        mapping.sqlite_code = SQLITE_INTERRUPT;
        mapping.message = "operation canceled";
        break;
    case VPS_ERROR_CLASS_POOL:
        mapping.sqlite_code = SQLITE_BUSY;
        mapping.message = "connection pool is busy";
        break;
    case VPS_ERROR_CLASS_SCHEMA:
        mapping.sqlite_code = SQLITE_SCHEMA;
        mapping.message = "remote schema changed";
        break;
    case VPS_ERROR_CLASS_MEMORY:
        mapping.sqlite_code = SQLITE_NOMEM;
        mapping.message = "out of memory";
        break;
    case VPS_ERROR_CLASS_UNSUPPORTED:
        mapping.message = "operation is unsupported";
        break;
    default:
        break;
    }
    return mapping;
}

static VpsMemoryResult vps_error_replace(
    VpsError *error,
    VpsErrorOperation operation,
    const char *sqlstate,
    int backend_status,
    int severity_class,
    const char *query_fingerprint,
    const VpsErrorMapping *mapping,
    const char *message)
{
    VpsOwnedMemory replacement;
    VpsOwnedMemory previous;
    VpsMemoryResult result;
    size_t message_size;

    result = vps_error_bounded_message_size(message, &message_size);
    if (result != VPS_MEMORY_OK) {
        return result;
    }
    result = vps_owned_memory_init(&replacement,
                                   &error->message_storage.allocator);
    if (result != VPS_MEMORY_OK) {
        return result;
    }
    if (mapping->error_class != VPS_ERROR_CLASS_MEMORY) {
        result = vps_owned_memory_allocate(&replacement, message_size + 1U);
        if (result != VPS_MEMORY_OK) {
            return result;
        }
        (void)memcpy(replacement.memory, message, message_size + 1U);
    }

    previous = error->message_storage;
    error->message_storage = replacement;
    error->sqlite_code = mapping->sqlite_code;
    error->sqlstate[0] = '\0';
    if (sqlstate != NULL) {
        (void)memcpy(error->sqlstate, sqlstate, VPS_SQLSTATE_BUFFER_SIZE);
    }
    error->backend_status = backend_status;
    error->severity_class = severity_class;
    error->operation = operation;
    error->query_fingerprint[0] = '\0';
    if (query_fingerprint != NULL && query_fingerprint[0] != '\0') {
        (void)memcpy(error->query_fingerprint, query_fingerprint,
                     VPS_ERROR_FINGERPRINT_BUFFER_SIZE);
    }
    error->error_class = mapping->error_class;
    error->transient = mapping->transient;
    error->ambiguous = mapping->ambiguous;
    vps_owned_memory_release(&previous);
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_error_init(VpsError *error,
                               const VpsAllocator *allocator)
{
    VpsMemoryResult result;

    if (error == NULL) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    result = vps_owned_memory_init(&error->message_storage, allocator);
    if (result != VPS_MEMORY_OK) {
        return result;
    }
    error->sqlite_code = SQLITE_OK;
    error->sqlstate[0] = '\0';
    error->backend_status = 0;
    error->severity_class = 0;
    error->operation = VPS_ERROR_OPERATION_NONE;
    error->query_fingerprint[0] = '\0';
    error->error_class = VPS_ERROR_CLASS_NONE;
    error->transient = 0;
    error->ambiguous = 0;
    error->initialized = 1;
    return VPS_MEMORY_OK;
}

void vps_error_reset(VpsError *error)
{
    if (error == NULL || !error->initialized) {
        return;
    }
    vps_owned_memory_release(&error->message_storage);
    error->sqlite_code = SQLITE_OK;
    error->sqlstate[0] = '\0';
    error->backend_status = 0;
    error->severity_class = 0;
    error->operation = VPS_ERROR_OPERATION_NONE;
    error->query_fingerprint[0] = '\0';
    error->error_class = VPS_ERROR_CLASS_NONE;
    error->transient = 0;
    error->ambiguous = 0;
}

VpsMemoryResult vps_error_set_sqlstate(
    VpsError *error,
    VpsErrorOperation operation,
    const char *sqlstate,
    int backend_status,
    int severity_class,
    const char *query_fingerprint)
{
    VpsErrorMapping mapping;

    if (error == NULL || !error->initialized ||
        !vps_error_operation_is_valid(operation) ||
        !vps_error_sqlstate_is_valid(sqlstate) ||
        !vps_error_fingerprint_is_valid(query_fingerprint)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    mapping = vps_error_map_sqlstate(operation, sqlstate);
    return vps_error_replace(error, operation, sqlstate, backend_status,
                             severity_class, query_fingerprint, &mapping,
                             mapping.message);
}

VpsMemoryResult vps_error_set_local(VpsError *error,
                                    VpsErrorOperation operation,
                                    VpsErrorClass error_class,
                                    const char *query_fingerprint)
{
    VpsErrorMapping mapping;

    if (error == NULL || !error->initialized ||
        !vps_error_operation_is_valid(operation) ||
        !vps_error_class_is_valid(error_class) ||
        !vps_error_fingerprint_is_valid(query_fingerprint)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    mapping = vps_error_map_local(operation, error_class);
    return vps_error_replace(error, operation, NULL, 0, 0,
                             query_fingerprint, &mapping, mapping.message);
}

VpsMemoryResult vps_error_copy(VpsError *destination,
                               const VpsError *source)
{
    VpsErrorMapping mapping;

    if (destination == NULL || source == NULL || !destination->initialized ||
        !source->initialized || destination == source) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (source->error_class == VPS_ERROR_CLASS_NONE) {
        vps_error_reset(destination);
        return VPS_MEMORY_OK;
    }
    if (!vps_error_operation_is_valid(source->operation) ||
        !vps_error_class_is_valid(source->error_class) ||
        !vps_error_fingerprint_is_valid(source->query_fingerprint) ||
        (source->sqlstate[0] != '\0' &&
         !vps_error_sqlstate_is_valid(source->sqlstate))) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    mapping.sqlite_code = source->sqlite_code;
    mapping.error_class = source->error_class;
    mapping.transient = source->transient;
    mapping.ambiguous = source->ambiguous;
    mapping.message = vps_error_message(source);
    return vps_error_replace(destination, source->operation,
                             source->sqlstate[0] == '\0' ? NULL
                                                         : source->sqlstate,
                             source->backend_status, source->severity_class,
                             source->query_fingerprint, &mapping,
                             mapping.message);
}

const char *vps_error_message(const VpsError *error)
{
    if (error == NULL || !error->initialized ||
        error->message_storage.memory == NULL) {
        if (error != NULL && error->initialized &&
            error->error_class == VPS_ERROR_CLASS_MEMORY) {
            return "out of memory";
        }
        return "";
    }
    return (const char *)error->message_storage.memory;
}

const char *vps_error_class_name(VpsErrorClass error_class)
{
    static const char *const names[] = {
        "none",          "config",      "auth",       "tls",
        "connection",    "timeout",     "cancel",     "pool",
        "sql",           "query_source", "metadata",  "schema",
        "constraint",    "serialization", "deadlock", "lock",
        "conversion",    "spatial",     "memory",     "transaction",
        "invariant",     "unsupported"};

    if (error_class < VPS_ERROR_CLASS_NONE ||
        error_class > VPS_ERROR_CLASS_UNSUPPORTED) {
        return "unknown";
    }
    return names[(size_t)error_class];
}

const char *vps_error_operation_name(VpsErrorOperation operation)
{
    static const char *const names[] = {
        "none",   "configure", "connect", "pool_wait",
        "metadata", "scan",    "query",   "dml",
        "cancel", "commit",    "rollback"};

    if (operation < VPS_ERROR_OPERATION_NONE ||
        operation > VPS_ERROR_OPERATION_ROLLBACK) {
        return "unknown";
    }
    return names[(size_t)operation];
}
