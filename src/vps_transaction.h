#ifndef VPS_TRANSACTION_H
#define VPS_TRANSACTION_H

#include "vps_identity.h"
#include "vps_error.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_TRANSACTION_MAX_PARTICIPANTS 64U
#define VPS_TRANSACTION_MAX_SAVEPOINTS 64U

typedef enum VpsTransactionState {
    VPS_TRANSACTION_IDLE = 0,
    VPS_TRANSACTION_BEGINNING = 1,
    VPS_TRANSACTION_ACTIVE = 2,
    VPS_TRANSACTION_FAILED = 3,
    VPS_TRANSACTION_ENDING = 4,
    VPS_TRANSACTION_AMBIGUOUS = 5
} VpsTransactionState;

typedef enum VpsTransactionResult {
    VPS_TRANSACTION_OK = 0,
    VPS_TRANSACTION_COMMAND_REQUIRED = 1,
    VPS_TRANSACTION_BUSY = 2,
    VPS_TRANSACTION_IDENTITY_MISMATCH = 3,
    VPS_TRANSACTION_INVALID_STATE = 4,
    VPS_TRANSACTION_ABORTED = 5,
    VPS_TRANSACTION_AMBIGUOUS_OUTCOME = 6,
    VPS_TRANSACTION_LIMIT_EXCEEDED = 7,
    VPS_TRANSACTION_OUT_OF_MEMORY = 8,
    VPS_TRANSACTION_INVALID_ARGUMENT = 9
} VpsTransactionResult;

typedef enum VpsTransactionEndOperation {
    VPS_TRANSACTION_END_COMMIT = 1,
    VPS_TRANSACTION_END_ROLLBACK = 2
} VpsTransactionEndOperation;

typedef struct VpsTransactionCoordinator {
    VpsAllocator allocator;
    VpsBuffer identity;
    VpsLogger *logger;
    uint64_t credential_generation;
    uint64_t configuration_generation;
    uint64_t generation;
    uint64_t participants[VPS_TRANSACTION_MAX_PARTICIPANTS];
    int savepoints[VPS_TRANSACTION_MAX_SAVEPOINTS];
    size_t participant_count;
    size_t savepoint_count;
    size_t active_stream_count;
    VpsTransactionState state;
    VpsTransactionEndOperation ending_operation;
    char identity_fingerprint[VPS_IDENTITY_FINGERPRINT_BUFFER_SIZE];
    char last_sqlstate[VPS_SQLSTATE_BUFFER_SIZE];
    int initialized;
} VpsTransactionCoordinator;

typedef struct VpsTransactionStatus {
    VpsTransactionState state;
    uint64_t generation;
    size_t participant_count;
    size_t savepoint_count;
    size_t active_stream_count;
    int ambiguous;
    char identity_fingerprint[VPS_IDENTITY_FINGERPRINT_BUFFER_SIZE];
    char last_sqlstate[VPS_SQLSTATE_BUFFER_SIZE];
} VpsTransactionStatus;

VpsTransactionResult vps_transaction_init(VpsTransactionCoordinator *coordinator,
                                          const VpsAllocator *allocator,
                                          VpsLogger *logger);
void vps_transaction_cleanup(VpsTransactionCoordinator *coordinator);
VpsTransactionResult vps_transaction_begin(
    VpsTransactionCoordinator *coordinator,
    const VpsConnectionIdentity *identity,
    uint64_t participant_id);
VpsTransactionResult vps_transaction_begin_complete(
    VpsTransactionCoordinator *coordinator, int success);
VpsTransactionResult vps_transaction_stream_begin(
    VpsTransactionCoordinator *coordinator);
VpsTransactionResult vps_transaction_stream_end(
    VpsTransactionCoordinator *coordinator);
VpsTransactionResult vps_transaction_command_allowed(
    const VpsTransactionCoordinator *coordinator);
VpsTransactionResult vps_transaction_mark_failed(
    VpsTransactionCoordinator *coordinator, const char *sqlstate);
VpsTransactionResult vps_transaction_savepoint(
    VpsTransactionCoordinator *coordinator, int level);
VpsTransactionResult vps_transaction_rollback_to(
    VpsTransactionCoordinator *coordinator, int level);
VpsTransactionResult vps_transaction_release(
    VpsTransactionCoordinator *coordinator, int level);
VpsTransactionResult vps_transaction_end(
    VpsTransactionCoordinator *coordinator,
    VpsTransactionEndOperation operation);
VpsTransactionResult vps_transaction_end_complete(
    VpsTransactionCoordinator *coordinator, int success,
    int connection_lost);
VpsTransactionResult vps_transaction_status(
    const VpsTransactionCoordinator *coordinator,
    VpsTransactionStatus *status);
const char *vps_transaction_state_name(VpsTransactionState state);
const char *vps_transaction_result_name(VpsTransactionResult result);

#endif
