#include "vps_transaction.h"

#include <string.h>

static void vps_transaction_log(VpsTransactionCoordinator *coordinator,
                                VpsLogLevel level,
                                const char *operation,
                                const char *outcome)
{
    VpsLogEvent event;
    if (coordinator == NULL || coordinator->logger == NULL) return;
    if (vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PROFILE,
                                 "transaction", 11U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                 operation, strlen(operation)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE,
                                 vps_transaction_state_name(coordinator->state),
                                 strlen(vps_transaction_state_name(coordinator->state))) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, outcome,
                                 strlen(outcome)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_GENERATION,
                                   coordinator->generation) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PARTICIPANT_COUNT,
                                   (uint64_t)coordinator->participant_count) != VPS_LOG_OK)
        return;
    vps_logger_emit(coordinator->logger, &event);
}

static void vps_transaction_reset_active(VpsTransactionCoordinator *coordinator)
{
    vps_buffer_reset(&coordinator->identity);
    (void)vps_buffer_init(&coordinator->identity, &coordinator->allocator,
                          VPS_IDENTITY_CANONICAL_LIMIT);
    coordinator->credential_generation = 0U;
    coordinator->configuration_generation = 0U;
    coordinator->participant_count = 0U;
    coordinator->savepoint_count = 0U;
    coordinator->active_stream_count = 0U;
    coordinator->ending_operation = (VpsTransactionEndOperation)0;
    coordinator->identity_fingerprint[0] = '\0';
    coordinator->last_sqlstate[0] = '\0';
    coordinator->state = VPS_TRANSACTION_IDLE;
}

VpsTransactionResult vps_transaction_init(VpsTransactionCoordinator *coordinator,
                                          const VpsAllocator *allocator,
                                          VpsLogger *logger)
{
    if (coordinator == NULL || !vps_allocator_is_valid(allocator))
        return VPS_TRANSACTION_INVALID_ARGUMENT;
    (void)memset(coordinator, 0, sizeof(*coordinator));
    coordinator->allocator = *allocator;
    coordinator->logger = logger;
    if (vps_buffer_init(&coordinator->identity, allocator,
                        VPS_IDENTITY_CANONICAL_LIMIT) != VPS_MEMORY_OK)
        return VPS_TRANSACTION_OUT_OF_MEMORY;
    coordinator->state = VPS_TRANSACTION_IDLE;
    coordinator->initialized = 1;
    return VPS_TRANSACTION_OK;
}

void vps_transaction_cleanup(VpsTransactionCoordinator *coordinator)
{
    if (coordinator == NULL) return;
    if (coordinator->initialized) vps_buffer_reset(&coordinator->identity);
    (void)memset(coordinator, 0, sizeof(*coordinator));
}

static int vps_transaction_has_participant(
    const VpsTransactionCoordinator *coordinator, uint64_t participant_id)
{
    size_t index;
    for (index = 0U; index < coordinator->participant_count; ++index)
        if (coordinator->participants[index] == participant_id) return 1;
    return 0;
}

static int vps_transaction_identity_matches(
    const VpsTransactionCoordinator *coordinator,
    const VpsConnectionIdentity *identity)
{
    return identity != NULL && identity->built &&
           coordinator->identity.size == identity->canonical.size &&
           coordinator->credential_generation == identity->credential_generation &&
           coordinator->configuration_generation == identity->configuration_generation &&
           (identity->canonical.size == 0U ||
            memcmp(coordinator->identity.data, identity->canonical.data,
                   identity->canonical.size) == 0);
}

VpsTransactionResult vps_transaction_begin(
    VpsTransactionCoordinator *coordinator,
    const VpsConnectionIdentity *identity,
    uint64_t participant_id)
{
    if (coordinator == NULL || !coordinator->initialized || identity == NULL ||
        !identity->built || participant_id == 0U)
        return VPS_TRANSACTION_INVALID_ARGUMENT;
    if (coordinator->state == VPS_TRANSACTION_AMBIGUOUS)
        return VPS_TRANSACTION_AMBIGUOUS_OUTCOME;
    if (coordinator->state == VPS_TRANSACTION_IDLE) {
        if (vps_buffer_append(&coordinator->identity, identity->canonical.data,
                              identity->canonical.size) != VPS_MEMORY_OK)
            return VPS_TRANSACTION_OUT_OF_MEMORY;
        coordinator->credential_generation = identity->credential_generation;
        coordinator->configuration_generation = identity->configuration_generation;
        (void)memcpy(coordinator->identity_fingerprint, identity->fingerprint,
                     sizeof(coordinator->identity_fingerprint));
        coordinator->participants[0] = participant_id;
        coordinator->participant_count = 1U;
        coordinator->generation += 1U;
        coordinator->state = VPS_TRANSACTION_BEGINNING;
        vps_transaction_log(coordinator, VPS_LOG_LEVEL_DEBUG, "begin", "required");
        return VPS_TRANSACTION_COMMAND_REQUIRED;
    }
    if (coordinator->state != VPS_TRANSACTION_ACTIVE &&
        coordinator->state != VPS_TRANSACTION_FAILED)
        return VPS_TRANSACTION_INVALID_STATE;
    if (!vps_transaction_identity_matches(coordinator, identity)) {
        vps_transaction_log(coordinator, VPS_LOG_LEVEL_WARN, "join", "identity_mismatch");
        return VPS_TRANSACTION_IDENTITY_MISMATCH;
    }
    if (vps_transaction_has_participant(coordinator, participant_id))
        return VPS_TRANSACTION_OK;
    if (coordinator->participant_count >= VPS_TRANSACTION_MAX_PARTICIPANTS)
        return VPS_TRANSACTION_LIMIT_EXCEEDED;
    coordinator->participants[coordinator->participant_count++] = participant_id;
    vps_transaction_log(coordinator, VPS_LOG_LEVEL_DEBUG, "join", "ok");
    return VPS_TRANSACTION_OK;
}

VpsTransactionResult vps_transaction_begin_complete(
    VpsTransactionCoordinator *coordinator, int success)
{
    if (coordinator == NULL || coordinator->state != VPS_TRANSACTION_BEGINNING)
        return VPS_TRANSACTION_INVALID_STATE;
    if (!success) {
        vps_transaction_reset_active(coordinator);
        return VPS_TRANSACTION_INVALID_STATE;
    }
    coordinator->state = VPS_TRANSACTION_ACTIVE;
    vps_transaction_log(coordinator, VPS_LOG_LEVEL_INFO, "begin", "complete");
    return VPS_TRANSACTION_OK;
}

VpsTransactionResult vps_transaction_stream_begin(
    VpsTransactionCoordinator *coordinator)
{
    if (coordinator == NULL || !coordinator->initialized)
        return VPS_TRANSACTION_INVALID_ARGUMENT;
    if (coordinator->state == VPS_TRANSACTION_FAILED)
        return VPS_TRANSACTION_ABORTED;
    if (coordinator->state != VPS_TRANSACTION_ACTIVE)
        return VPS_TRANSACTION_INVALID_STATE;
    if (coordinator->active_stream_count != 0U) return VPS_TRANSACTION_BUSY;
    coordinator->active_stream_count = 1U;
    vps_transaction_log(coordinator, VPS_LOG_LEVEL_DEBUG, "stream", "acquired");
    return VPS_TRANSACTION_OK;
}

VpsTransactionResult vps_transaction_stream_end(
    VpsTransactionCoordinator *coordinator)
{
    if (coordinator == NULL || !coordinator->initialized ||
        coordinator->active_stream_count == 0U)
        return VPS_TRANSACTION_INVALID_STATE;
    coordinator->active_stream_count = 0U;
    vps_transaction_log(coordinator, VPS_LOG_LEVEL_DEBUG, "stream", "released");
    return VPS_TRANSACTION_OK;
}

VpsTransactionResult vps_transaction_command_allowed(
    const VpsTransactionCoordinator *coordinator)
{
    if (coordinator == NULL || !coordinator->initialized)
        return VPS_TRANSACTION_INVALID_ARGUMENT;
    if (coordinator->state == VPS_TRANSACTION_FAILED)
        return VPS_TRANSACTION_ABORTED;
    if (coordinator->state == VPS_TRANSACTION_AMBIGUOUS)
        return VPS_TRANSACTION_AMBIGUOUS_OUTCOME;
    if (coordinator->state != VPS_TRANSACTION_ACTIVE)
        return VPS_TRANSACTION_INVALID_STATE;
    return coordinator->active_stream_count == 0U ? VPS_TRANSACTION_OK
                                                   : VPS_TRANSACTION_BUSY;
}

VpsTransactionResult vps_transaction_mark_failed(
    VpsTransactionCoordinator *coordinator, const char *sqlstate)
{
    if (coordinator == NULL || !coordinator->initialized ||
        (coordinator->state != VPS_TRANSACTION_ACTIVE &&
         coordinator->state != VPS_TRANSACTION_FAILED))
        return VPS_TRANSACTION_INVALID_STATE;
    coordinator->state = VPS_TRANSACTION_FAILED;
    if (sqlstate != NULL && strlen(sqlstate) == VPS_SQLSTATE_LENGTH) {
        (void)memcpy(coordinator->last_sqlstate, sqlstate, VPS_SQLSTATE_LENGTH);
        coordinator->last_sqlstate[VPS_SQLSTATE_LENGTH] = '\0';
    }
    vps_transaction_log(coordinator, VPS_LOG_LEVEL_WARN, "state", "aborted");
    return VPS_TRANSACTION_OK;
}

static ptrdiff_t vps_transaction_find_savepoint(
    const VpsTransactionCoordinator *coordinator, int level)
{
    size_t index;
    for (index = 0U; index < coordinator->savepoint_count; ++index)
        if (coordinator->savepoints[index] == level) return (ptrdiff_t)index;
    return -1;
}

VpsTransactionResult vps_transaction_savepoint(
    VpsTransactionCoordinator *coordinator, int level)
{
    if (level < 0) return VPS_TRANSACTION_INVALID_ARGUMENT;
    if (vps_transaction_command_allowed(coordinator) != VPS_TRANSACTION_OK)
        return vps_transaction_command_allowed(coordinator);
    if (vps_transaction_find_savepoint(coordinator, level) >= 0)
        return VPS_TRANSACTION_OK;
    if (coordinator->savepoint_count >= VPS_TRANSACTION_MAX_SAVEPOINTS)
        return VPS_TRANSACTION_LIMIT_EXCEEDED;
    coordinator->savepoints[coordinator->savepoint_count++] = level;
    return VPS_TRANSACTION_COMMAND_REQUIRED;
}

VpsTransactionResult vps_transaction_rollback_to(
    VpsTransactionCoordinator *coordinator, int level)
{
    ptrdiff_t found;
    if (coordinator == NULL || !coordinator->initialized || level < 0)
        return VPS_TRANSACTION_INVALID_ARGUMENT;
    if (coordinator->active_stream_count != 0U) return VPS_TRANSACTION_BUSY;
    if (coordinator->state != VPS_TRANSACTION_ACTIVE &&
        coordinator->state != VPS_TRANSACTION_FAILED)
        return VPS_TRANSACTION_INVALID_STATE;
    found = vps_transaction_find_savepoint(coordinator, level);
    if (found < 0) return VPS_TRANSACTION_INVALID_STATE;
    coordinator->savepoint_count = (size_t)found + 1U;
    coordinator->state = VPS_TRANSACTION_ACTIVE;
    coordinator->last_sqlstate[0] = '\0';
    return VPS_TRANSACTION_COMMAND_REQUIRED;
}

VpsTransactionResult vps_transaction_release(
    VpsTransactionCoordinator *coordinator, int level)
{
    ptrdiff_t found;
    VpsTransactionResult allowed = vps_transaction_command_allowed(coordinator);
    if (level < 0) return VPS_TRANSACTION_INVALID_ARGUMENT;
    if (allowed != VPS_TRANSACTION_OK) return allowed;
    found = vps_transaction_find_savepoint(coordinator, level);
    if (found < 0) return VPS_TRANSACTION_OK;
    coordinator->savepoint_count = (size_t)found;
    return VPS_TRANSACTION_COMMAND_REQUIRED;
}

VpsTransactionResult vps_transaction_end(
    VpsTransactionCoordinator *coordinator,
    VpsTransactionEndOperation operation)
{
    if (coordinator == NULL || !coordinator->initialized ||
        (operation != VPS_TRANSACTION_END_COMMIT &&
         operation != VPS_TRANSACTION_END_ROLLBACK))
        return VPS_TRANSACTION_INVALID_ARGUMENT;
    if (coordinator->state == VPS_TRANSACTION_IDLE) return VPS_TRANSACTION_OK;
    if (coordinator->state == VPS_TRANSACTION_AMBIGUOUS)
        return VPS_TRANSACTION_AMBIGUOUS_OUTCOME;
    if (coordinator->active_stream_count != 0U) return VPS_TRANSACTION_BUSY;
    if (operation == VPS_TRANSACTION_END_COMMIT &&
        coordinator->state == VPS_TRANSACTION_FAILED)
        return VPS_TRANSACTION_ABORTED;
    if (coordinator->state != VPS_TRANSACTION_ACTIVE &&
        !(operation == VPS_TRANSACTION_END_ROLLBACK &&
          coordinator->state == VPS_TRANSACTION_FAILED))
        return VPS_TRANSACTION_INVALID_STATE;
    coordinator->state = VPS_TRANSACTION_ENDING;
    coordinator->ending_operation = operation;
    return VPS_TRANSACTION_COMMAND_REQUIRED;
}

VpsTransactionResult vps_transaction_end_complete(
    VpsTransactionCoordinator *coordinator, int success,
    int connection_lost)
{
    if (coordinator == NULL || coordinator->state != VPS_TRANSACTION_ENDING)
        return VPS_TRANSACTION_INVALID_STATE;
    if (success) {
        vps_transaction_log(coordinator, VPS_LOG_LEVEL_INFO,
                            coordinator->ending_operation == VPS_TRANSACTION_END_COMMIT
                                ? "commit" : "rollback", "complete");
        vps_transaction_reset_active(coordinator);
        return VPS_TRANSACTION_OK;
    }
    if (connection_lost) {
        coordinator->state = VPS_TRANSACTION_AMBIGUOUS;
        coordinator->active_stream_count = 0U;
        vps_transaction_log(coordinator, VPS_LOG_LEVEL_ERROR, "end", "ambiguous");
        return VPS_TRANSACTION_AMBIGUOUS_OUTCOME;
    }
    coordinator->state = VPS_TRANSACTION_FAILED;
    return VPS_TRANSACTION_ABORTED;
}

VpsTransactionResult vps_transaction_status(
    const VpsTransactionCoordinator *coordinator,
    VpsTransactionStatus *status)
{
    if (coordinator == NULL || !coordinator->initialized || status == NULL)
        return VPS_TRANSACTION_INVALID_ARGUMENT;
    (void)memset(status, 0, sizeof(*status));
    status->state = coordinator->state;
    status->generation = coordinator->generation;
    status->participant_count = coordinator->participant_count;
    status->savepoint_count = coordinator->savepoint_count;
    status->active_stream_count = coordinator->active_stream_count;
    status->ambiguous = coordinator->state == VPS_TRANSACTION_AMBIGUOUS;
    (void)memcpy(status->identity_fingerprint,
                 coordinator->identity_fingerprint,
                 sizeof(status->identity_fingerprint));
    (void)memcpy(status->last_sqlstate, coordinator->last_sqlstate,
                 sizeof(status->last_sqlstate));
    return VPS_TRANSACTION_OK;
}

const char *vps_transaction_state_name(VpsTransactionState state)
{
    switch (state) {
    case VPS_TRANSACTION_IDLE: return "idle";
    case VPS_TRANSACTION_BEGINNING: return "beginning";
    case VPS_TRANSACTION_ACTIVE: return "active";
    case VPS_TRANSACTION_FAILED: return "failed";
    case VPS_TRANSACTION_ENDING: return "ending";
    case VPS_TRANSACTION_AMBIGUOUS: return "ambiguous";
    default: return "unknown";
    }
}

const char *vps_transaction_result_name(VpsTransactionResult result)
{
    switch (result) {
    case VPS_TRANSACTION_OK: return "ok";
    case VPS_TRANSACTION_COMMAND_REQUIRED: return "command_required";
    case VPS_TRANSACTION_BUSY: return "busy";
    case VPS_TRANSACTION_IDENTITY_MISMATCH: return "identity_mismatch";
    case VPS_TRANSACTION_INVALID_STATE: return "invalid_state";
    case VPS_TRANSACTION_ABORTED: return "aborted";
    case VPS_TRANSACTION_AMBIGUOUS_OUTCOME: return "ambiguous";
    case VPS_TRANSACTION_LIMIT_EXCEEDED: return "limit_exceeded";
    case VPS_TRANSACTION_OUT_OF_MEMORY: return "out_of_memory";
    case VPS_TRANSACTION_INVALID_ARGUMENT: return "invalid_argument";
    default: return "unknown";
    }
}
