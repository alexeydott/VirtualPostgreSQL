#ifndef VPS_CLIENT_H
#define VPS_CLIENT_H

#include "vps_error.h"
#include "vps_deadline.h"
#include "vps_logging.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_CLIENT_CONTRACT_VERSION UINT32_C(2)
#define VPS_CLIENT_WAIT_MAX_SLICE_MS UINT32_C(1000)
#define VPS_CLIENT_MAX_QUERY_BYTES ((1024U * 1024U) + 128U)
#define VPS_CLIENT_MAX_PARAMETER_COUNT 65535U
#define VPS_CLIENT_MAX_RESULT_FIELD_COUNT 4096U
#define VPS_CLIENT_MAX_STATEMENT_TIMEOUT_MS UINT64_C(86400000)
#define VPS_CLIENT_MAX_COLUMN_BYTES (16U * 1024U * 1024U)
#define VPS_CLIENT_MAX_ROW_BYTES (64U * 1024U * 1024U)
#define VPS_CLIENT_MAX_FIELD_NAME_BYTES 255U

#define VPS_CLIENT_CAP_CONNECT     (UINT64_C(1) << 0)
#define VPS_CLIENT_CAP_PREPARE     (UINT64_C(1) << 1)
#define VPS_CLIENT_CAP_EXECUTE     (UINT64_C(1) << 2)
#define VPS_CLIENT_CAP_FETCH       (UINT64_C(1) << 3)
#define VPS_CLIENT_CAP_TRANSACTION (UINT64_C(1) << 4)
#define VPS_CLIENT_CAP_RESET       (UINT64_C(1) << 5)
#define VPS_CLIENT_CAP_PING        (UINT64_C(1) << 6)
#define VPS_CLIENT_CAP_CANCEL      (UINT64_C(1) << 7)
#define VPS_CLIENT_CAP_ALL         UINT64_C(0xff)

typedef enum VpsClientStatus {
    VPS_CLIENT_OK = 0,
    VPS_CLIENT_INVALID_ARGUMENT = 1,
    VPS_CLIENT_INVALID_STATE = 2,
    VPS_CLIENT_UNSUPPORTED = 3,
    VPS_CLIENT_OUT_OF_MEMORY = 4,
    VPS_CLIENT_LIMIT_EXCEEDED = 5,
    VPS_CLIENT_BACKEND_ERROR = 6,
    /* Interrupt/deadline observed while backend operation remains cancellable. */
    VPS_CLIENT_CONTROL_SIGNAL = 7
} VpsClientStatus;

typedef enum VpsClientOperation {
    VPS_CLIENT_OPERATION_NONE = 0,
    VPS_CLIENT_OPERATION_CONNECT = 1,
    VPS_CLIENT_OPERATION_PREPARE = 2,
    VPS_CLIENT_OPERATION_EXECUTE = 3,
    VPS_CLIENT_OPERATION_FETCH = 4,
    VPS_CLIENT_OPERATION_BEGIN = 5,
    VPS_CLIENT_OPERATION_COMMIT = 6,
    VPS_CLIENT_OPERATION_ROLLBACK = 7,
    VPS_CLIENT_OPERATION_RESET = 8,
    VPS_CLIENT_OPERATION_PING = 9,
    VPS_CLIENT_OPERATION_CANCEL = 10
} VpsClientOperation;

typedef enum VpsClientConnectionState {
    VPS_CLIENT_CONNECTION_NEW = 0,
    VPS_CLIENT_CONNECTION_CONNECTING = 1,
    VPS_CLIENT_CONNECTION_READY = 2,
    VPS_CLIENT_CONNECTION_TRANSACTION_STARTING = 3,
    VPS_CLIENT_CONNECTION_TRANSACTION_ACTIVE = 4,
    VPS_CLIENT_CONNECTION_TRANSACTION_ENDING = 5,
    VPS_CLIENT_CONNECTION_RESETTING = 6,
    VPS_CLIENT_CONNECTION_PINGING = 7,
    VPS_CLIENT_CONNECTION_CANCELLING = 8,
    VPS_CLIENT_CONNECTION_FAILED = 9
} VpsClientConnectionState;

typedef enum VpsClientStatementState {
    VPS_CLIENT_STATEMENT_NEW = 0,
    VPS_CLIENT_STATEMENT_PREPARING = 1,
    VPS_CLIENT_STATEMENT_PREPARED = 2,
    VPS_CLIENT_STATEMENT_EXECUTING = 3,
    VPS_CLIENT_STATEMENT_FETCHING = 4,
    VPS_CLIENT_STATEMENT_ROW_READY = 5,
    VPS_CLIENT_STATEMENT_COMPLETE = 6,
    VPS_CLIENT_STATEMENT_CANCELLING = 7,
    VPS_CLIENT_STATEMENT_FAILED = 8
} VpsClientStatementState;

typedef enum VpsClientPollOutcome {
    VPS_CLIENT_POLL_WAIT = 0,
    VPS_CLIENT_POLL_COMPLETE = 1,
    VPS_CLIENT_POLL_ROW_READY = 2,
    VPS_CLIENT_POLL_FAILED = 3
} VpsClientPollOutcome;

typedef enum VpsClientWaitInterest {
    VPS_CLIENT_WAIT_READ = 1,
    VPS_CLIENT_WAIT_WRITE = 2,
    VPS_CLIENT_WAIT_READ_WRITE = 3
} VpsClientWaitInterest;

typedef enum VpsClientWaitPhase {
    VPS_CLIENT_WAIT_CONNECT = 0,
    VPS_CLIENT_WAIT_PREPARE = 1,
    VPS_CLIENT_WAIT_EXECUTE = 2,
    VPS_CLIENT_WAIT_FETCH = 3,
    VPS_CLIENT_WAIT_TRANSACTION = 4,
    VPS_CLIENT_WAIT_RESET = 5,
    VPS_CLIENT_WAIT_PING = 6,
    VPS_CLIENT_WAIT_CANCEL = 7
} VpsClientWaitPhase;

/*
 * This logical wait descriptor contains no socket or platform handle. The
 * backend adapter owns the native descriptor and maps this bounded request to
 * its deadline/wait implementation. max_slice_ms is always in
 * [1, VPS_CLIENT_WAIT_MAX_SLICE_MS].
 */
typedef struct VpsClientWaitRequest {
    VpsClientWaitInterest interest;
    VpsClientWaitPhase phase;
    uint32_t max_slice_ms;
} VpsClientWaitRequest;

typedef struct VpsClientPollResult {
    VpsClientPollOutcome outcome;
    VpsClientWaitRequest wait;
} VpsClientPollResult;

typedef enum VpsClientValueFormat {
    VPS_CLIENT_VALUE_TEXT = 0,
    VPS_CLIENT_VALUE_BINARY = 1
} VpsClientValueFormat;

/*
 * Parameter bytes are borrowed through statement close. NULL is represented
 * only by is_null; a non-NULL zero-length value is distinct. length is
 * checked before the backend narrows it to its native integer width.
 */
typedef struct VpsClientParameterView {
    const void *value;
    size_t length;
    uint32_t type_oid;
    VpsClientValueFormat format;
    int is_null;
} VpsClientParameterView;

typedef struct VpsClientResultFieldExpectation {
    uint32_t type_oid;
    VpsClientValueFormat format;
} VpsClientResultFieldExpectation;

/*
 * The query, parameter array, parameter bytes and expected-field array are
 * borrowed and must outlive the statement. query is NUL-terminated and
 * query_length excludes that terminator. prepare selects PQsendPrepare plus
 * describe before execution; otherwise execution uses extended parameters
 * directly. No descriptor or value is logged.
 */
typedef struct VpsClientStatementSpec {
    const char *query;
    size_t query_length;
    const VpsClientParameterView *parameters;
    size_t parameter_count;
    const VpsClientResultFieldExpectation *result_fields;
    size_t result_field_count;
    uint64_t timeout_ms;
    int prepare;
    int single_row;
    /* Prepare/describe an initially unknown result layout without execution. */
    int discover_result_fields;
    /* Optional statement-scoped probe overrides the client-wide probe. */
    VpsInterruptProbe interrupt_probe;
    void *interrupt_context;
    /* NONE preserves QUERY mapping; DML selects constraint-aware mapping. */
    VpsErrorOperation error_operation;
} VpsClientStatementSpec;

typedef struct VpsClientStatementMetadata {
    size_t parameter_count;
    size_t result_field_count;
    uint64_t query_fingerprint;
    uint64_t published_row_count;
    uint64_t affected_count;
    int affected_count_valid;
    int described;
} VpsClientStatementMetadata;

/* Borrowed descriptor; name remains valid until statement close. */
typedef struct VpsClientResultFieldMetadata {
    const char *name;
    size_t name_length;
    uint32_t type_oid;
    int32_t type_modifier;
    uint32_t origin_relation_oid;
    int32_t origin_attribute_number;
    VpsClientValueFormat format;
} VpsClientResultFieldMetadata;

typedef struct VpsClientColumnView {
    const void *data;
    size_t length;
    uint32_t type_oid;
    VpsClientValueFormat format;
    int is_null;
} VpsClientColumnView;

typedef struct VpsClientConnection VpsClientConnection;
typedef struct VpsClientStatement VpsClientStatement;
typedef struct VpsClientResult VpsClientResult;
typedef struct VpsClientRowView VpsClientRowView;

typedef VpsClientStatus (*VpsClientConnectionCreateFunction)(
    void *context,
    void **backend_connection,
    VpsError *error);
typedef VpsClientStatus (*VpsClientConnectionStartFunction)(
    void *context,
    void *backend_connection,
    VpsClientOperation operation,
    VpsError *error);
typedef VpsClientStatus (*VpsClientConnectionPollFunction)(
    void *context,
    void *backend_connection,
    VpsClientPollResult *poll_result,
    VpsError *error);
typedef VpsClientStatus (*VpsClientConnectionWaitFunction)(
    void *context,
    void *backend_connection,
    const VpsClientWaitRequest *wait_request,
    VpsError *error);
typedef void (*VpsClientConnectionDestroyFunction)(
    void *context,
    void *backend_connection);

typedef VpsClientStatus (*VpsClientStatementCreateFunction)(
    void *context,
    void *backend_connection,
    const VpsClientStatementSpec *spec,
    void **backend_statement,
    VpsError *error);
typedef VpsClientStatus (*VpsClientStatementStartFunction)(
    void *context,
    void *backend_statement,
    VpsClientOperation operation,
    VpsError *error);
typedef VpsClientStatus (*VpsClientStatementPollFunction)(
    void *context,
    void *backend_statement,
    VpsClientPollResult *poll_result,
    VpsError *error);
typedef VpsClientStatus (*VpsClientStatementWaitFunction)(
    void *context,
    void *backend_statement,
    const VpsClientWaitRequest *wait_request,
    VpsError *error);
typedef VpsClientStatus (*VpsClientStatementMetadataFunction)(
    void *context,
    const void *backend_statement,
    VpsClientStatementMetadata *metadata,
    VpsError *error);
typedef VpsClientStatus (*VpsClientStatementResultFieldFunction)(
    void *context,
    const void *backend_statement,
    size_t field_index,
    VpsClientResultFieldMetadata *field,
    VpsError *error);
typedef VpsClientStatus (*VpsClientStatementRowFunction)(
    void *context,
    const void *backend_statement,
    size_t *column_count,
    VpsError *error);
typedef VpsClientStatus (*VpsClientStatementColumnFunction)(
    void *context,
    const void *backend_statement,
    size_t column_index,
    VpsClientColumnView *column,
    VpsError *error);
typedef void (*VpsClientStatementRowReleaseFunction)(
    void *context,
    void *backend_statement);
typedef void (*VpsClientStatementDestroyFunction)(
    void *context,
    void *backend_statement);

/*
 * The operations table is copied by vps_client_init. Backend context is
 * borrowed and must outlive the client and every child object. Create
 * callbacks transfer a non-NULL backend handle on success. If a failing
 * create callback publishes a partial handle, the port destroys it exactly
 * once before returning. Destroy callbacks must accept only owned non-NULL
 * handles. Callbacks and child objects are caller-serialized in version 1.
 */
typedef struct VpsClientOperations {
    uint32_t structure_size;
    uint32_t contract_version;
    uint64_t capabilities;
    VpsClientConnectionCreateFunction connection_create;
    VpsClientConnectionStartFunction connection_start;
    VpsClientConnectionPollFunction connection_poll;
    VpsClientConnectionWaitFunction connection_wait;
    VpsClientConnectionDestroyFunction connection_destroy;
    VpsClientStatementCreateFunction statement_create;
    VpsClientStatementStartFunction statement_start;
    VpsClientStatementPollFunction statement_poll;
    VpsClientStatementWaitFunction statement_wait;
    VpsClientStatementMetadataFunction statement_metadata;
    VpsClientStatementResultFieldFunction statement_result_field;
    VpsClientStatementRowFunction statement_row;
    VpsClientStatementColumnFunction statement_column;
    VpsClientStatementRowReleaseFunction statement_row_release;
    VpsClientStatementDestroyFunction statement_destroy;
} VpsClientOperations;

/*
 * VpsClient is stack-owned and not thread-safe. It copies allocator and
 * operations dispatch, borrows logger/backend context, and must outlive all
 * connections/statements created from it. cleanup is idempotent only when no
 * child connection remains; otherwise it returns INVALID_STATE unchanged.
 */
typedef struct VpsClient {
    VpsAllocator allocator;
    VpsClientOperations operations;
    void *backend_context;
    VpsLogger *logger;
    size_t connection_count;
    int initialized;
} VpsClient;

VpsClientStatus vps_client_init(VpsClient *client,
                                const VpsAllocator *allocator,
                                const VpsClientOperations *operations,
                                void *backend_context,
                                VpsLogger *logger);
VpsClientStatus vps_client_cleanup(VpsClient *client);

VpsClientStatus vps_client_connection_open(
    VpsClient *client,
    VpsClientConnection **connection,
    VpsError *error);
VpsClientStatus vps_client_connection_start(
    VpsClientConnection *connection,
    VpsClientOperation operation,
    VpsError *error);
VpsClientStatus vps_client_connection_poll(
    VpsClientConnection *connection,
    VpsClientPollResult *poll_result,
    VpsError *error);
/* May block only within the backend-owned monotonic deadline. */
VpsClientStatus vps_client_connection_wait(
    VpsClientConnection *connection,
    const VpsClientWaitRequest *wait_request,
    VpsError *error);
VpsClientStatus vps_client_connection_close(
    VpsClientConnection **connection);

VpsClientStatus vps_client_statement_open(
    VpsClientConnection *connection,
    const VpsClientStatementSpec *spec,
    VpsClientStatement **statement,
    VpsError *error);
VpsClientStatus vps_client_statement_start(
    VpsClientStatement *statement,
    VpsClientOperation operation,
    VpsError *error);
VpsClientStatus vps_client_statement_poll(
    VpsClientStatement *statement,
    VpsClientPollResult *poll_result,
    VpsError *error);
/* May block only within the backend-owned monotonic deadline. */
VpsClientStatus vps_client_statement_wait(
    VpsClientStatement *statement,
    const VpsClientWaitRequest *wait_request,
    VpsError *error);
VpsClientStatus vps_client_statement_metadata(
    const VpsClientStatement *statement,
    VpsClientStatementMetadata *metadata,
    VpsError *error);
VpsClientStatus vps_client_statement_result_field(
    const VpsClientStatement *statement,
    size_t field_index,
    VpsClientResultFieldMetadata *field,
    VpsError *error);
VpsClientStatus vps_client_statement_current_row(
    VpsClientStatement *statement,
    const VpsClientRowView **row,
    VpsError *error);
size_t vps_client_row_column_count(const VpsClientRowView *row);
VpsClientStatus vps_client_row_column(
    const VpsClientRowView *row,
    size_t column_index,
    VpsClientColumnView *column,
    VpsError *error);
VpsClientStatus vps_client_statement_row_consumed(
    VpsClientStatement *statement,
    VpsError *error);
VpsClientStatus vps_client_statement_close(
    VpsClientStatement **statement);

VpsClientConnectionState vps_client_connection_state(
    const VpsClientConnection *connection);
VpsClientStatementState vps_client_statement_state(
    const VpsClientStatement *statement);
uint64_t vps_client_statement_row_generation(
    const VpsClientStatement *statement);
int vps_client_statement_row_is_current(
    const VpsClientStatement *statement,
    uint64_t generation);

const char *vps_client_status_name(VpsClientStatus status);
const char *vps_client_operation_name(VpsClientOperation operation);
const char *vps_client_connection_state_name(VpsClientConnectionState state);
const char *vps_client_statement_state_name(VpsClientStatementState state);
const char *vps_client_poll_outcome_name(VpsClientPollOutcome outcome);
const char *vps_client_wait_phase_name(VpsClientWaitPhase phase);

#endif
