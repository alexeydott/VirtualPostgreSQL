#include "vps_transaction.h"

#include <stdio.h>
#include <string.h>

static int check(int condition, const char *name)
{
    if (!condition) (void)fprintf(stderr, "transaction_test failed: %s\n", name);
    return condition;
}

static int make_identity(VpsConnectionIdentity *identity,
                         const VpsAllocator *allocator,
                         const char *bytes, uint64_t generation)
{
    (void)memset(identity, 0, sizeof(*identity));
    if (vps_buffer_init(&identity->canonical, allocator, 128U) != VPS_MEMORY_OK ||
        vps_buffer_append(&identity->canonical, bytes, strlen(bytes)) != VPS_MEMORY_OK)
        return 0;
    identity->credential_generation = generation;
    identity->configuration_generation = generation;
    (void)snprintf(identity->fingerprint, sizeof(identity->fingerprint),
                   "identity-%llu", (unsigned long long)generation);
    identity->initialized = 1;
    identity->built = 1;
    return 1;
}

int main(void)
{
    VpsAllocator allocator;
    VpsTransactionCoordinator coordinator;
    VpsTransactionStatus status;
    VpsConnectionIdentity first;
    VpsConnectionIdentity same;
    VpsConnectionIdentity other;
    int ok = 1;
    ok &= check(vps_allocator_system(&allocator) == VPS_MEMORY_OK, "allocator");
    ok &= check(make_identity(&first, &allocator, "same", 7U), "first identity");
    ok &= check(make_identity(&same, &allocator, "same", 7U), "same identity");
    ok &= check(make_identity(&other, &allocator, "other", 8U), "other identity");
    ok &= check(vps_transaction_init(&coordinator, &allocator, NULL) == VPS_TRANSACTION_OK,
                "init");

    ok &= check(vps_transaction_begin(&coordinator, &first, 11U) ==
                    VPS_TRANSACTION_COMMAND_REQUIRED, "first begin");
    ok &= check(vps_transaction_begin_complete(&coordinator, 1) == VPS_TRANSACTION_OK,
                "begin complete");
    ok &= check(vps_transaction_begin(&coordinator, &same, 12U) == VPS_TRANSACTION_OK,
                "compatible join");
    ok &= check(vps_transaction_begin(&coordinator, &same, 12U) == VPS_TRANSACTION_OK,
                "duplicate join");
    ok &= check(vps_transaction_begin(&coordinator, &other, 13U) ==
                    VPS_TRANSACTION_IDENTITY_MISMATCH, "identity mismatch");
    ok &= check(vps_transaction_status(&coordinator, &status) == VPS_TRANSACTION_OK &&
                    status.participant_count == 2U, "participant status");

    ok &= check(vps_transaction_savepoint(&coordinator, 0) ==
                    VPS_TRANSACTION_COMMAND_REQUIRED, "savepoint zero");
    ok &= check(vps_transaction_savepoint(&coordinator, 0) == VPS_TRANSACTION_OK,
                "duplicate savepoint");
    ok &= check(vps_transaction_savepoint(&coordinator, 1) ==
                    VPS_TRANSACTION_COMMAND_REQUIRED, "nested savepoint");
    ok &= check(vps_transaction_stream_begin(&coordinator) == VPS_TRANSACTION_OK,
                "stream begin");
    ok &= check(vps_transaction_stream_begin(&coordinator) == VPS_TRANSACTION_BUSY,
                "second stream busy");
    ok &= check(vps_transaction_command_allowed(&coordinator) == VPS_TRANSACTION_BUSY,
                "dml busy");
    ok &= check(vps_transaction_savepoint(&coordinator, 2) == VPS_TRANSACTION_BUSY,
                "savepoint busy");
    ok &= check(vps_transaction_end(&coordinator, VPS_TRANSACTION_END_COMMIT) ==
                    VPS_TRANSACTION_BUSY, "commit busy");
    ok &= check(vps_transaction_stream_end(&coordinator) == VPS_TRANSACTION_OK,
                "stream end");

    ok &= check(vps_transaction_mark_failed(&coordinator, "23505") == VPS_TRANSACTION_OK,
                "mark aborted");
    ok &= check(vps_transaction_command_allowed(&coordinator) == VPS_TRANSACTION_ABORTED,
                "dml aborted");
    ok &= check(vps_transaction_end(&coordinator, VPS_TRANSACTION_END_COMMIT) ==
                    VPS_TRANSACTION_ABORTED, "commit aborted");
    ok &= check(vps_transaction_rollback_to(&coordinator, 0) ==
                    VPS_TRANSACTION_COMMAND_REQUIRED, "rollback-to recovers");
    ok &= check(vps_transaction_status(&coordinator, &status) == VPS_TRANSACTION_OK &&
                    status.state == VPS_TRANSACTION_ACTIVE &&
                    status.savepoint_count == 1U, "rollback-to state");
    ok &= check(vps_transaction_release(&coordinator, 0) ==
                    VPS_TRANSACTION_COMMAND_REQUIRED, "release");
    ok &= check(vps_transaction_release(&coordinator, 0) == VPS_TRANSACTION_OK,
                "duplicate release");

    ok &= check(vps_transaction_end(&coordinator, VPS_TRANSACTION_END_ROLLBACK) ==
                    VPS_TRANSACTION_COMMAND_REQUIRED, "rollback request");
    ok &= check(vps_transaction_end_complete(&coordinator, 1, 0) == VPS_TRANSACTION_OK,
                "rollback complete");
    ok &= check(vps_transaction_status(&coordinator, &status) == VPS_TRANSACTION_OK &&
                    status.state == VPS_TRANSACTION_IDLE, "idle after rollback");

    ok &= check(vps_transaction_begin(&coordinator, &first, 11U) ==
                    VPS_TRANSACTION_COMMAND_REQUIRED &&
                    vps_transaction_begin_complete(&coordinator, 1) == VPS_TRANSACTION_OK,
                "second transaction");
    ok &= check(vps_transaction_end(&coordinator, VPS_TRANSACTION_END_COMMIT) ==
                    VPS_TRANSACTION_COMMAND_REQUIRED, "commit request");
    ok &= check(vps_transaction_end_complete(&coordinator, 0, 1) ==
                    VPS_TRANSACTION_AMBIGUOUS_OUTCOME, "commit loss ambiguous");
    ok &= check(vps_transaction_status(&coordinator, &status) == VPS_TRANSACTION_OK &&
                    status.ambiguous, "ambiguous status");
    ok &= check(vps_transaction_begin(&coordinator, &first, 11U) ==
                    VPS_TRANSACTION_AMBIGUOUS_OUTCOME, "ambiguous terminal");

    vps_transaction_cleanup(&coordinator);
    vps_identity_cleanup(&first);
    vps_identity_cleanup(&same);
    vps_identity_cleanup(&other);
    if (!ok) return 1;
    (void)printf("transaction_coordinator status=passed\n");
    return 0;
}
