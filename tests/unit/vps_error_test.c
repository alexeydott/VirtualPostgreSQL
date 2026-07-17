#include "vps_error.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VpsErrorAllocatorState {
    size_t allocations;
    size_t deallocations;
} VpsErrorAllocatorState;

typedef struct VpsSqlstateCase {
    const char *name;
    const char *sqlstate;
    VpsErrorOperation operation;
    int sqlite_code;
    VpsErrorClass error_class;
    int transient;
    int ambiguous;
} VpsSqlstateCase;

static void *vps_error_test_allocate(void *context, size_t size)
{
    VpsErrorAllocatorState *state = (VpsErrorAllocatorState *)context;
    state->allocations += 1U;
    return malloc(size);
}

static void *vps_error_test_reallocate(void *context,
                                       void *memory,
                                       size_t old_size,
                                       size_t new_size)
{
    (void)context;
    (void)old_size;
    return realloc(memory, new_size);
}

static void vps_error_test_deallocate(void *context,
                                      void *memory,
                                      size_t size)
{
    VpsErrorAllocatorState *state = (VpsErrorAllocatorState *)context;
    (void)size;
    state->deallocations += 1U;
    free(memory);
}

static int vps_error_expect(int condition, const char *case_name)
{
    if (!condition) {
        (void)fprintf(stderr,
                      "[error] level=error case=%s status=failed\n",
                      case_name);
        return 0;
    }
    return 1;
}

static int vps_error_test_sqlstate_table(void)
{
    static const VpsSqlstateCase cases[] = {
        {"unique", "23505", VPS_ERROR_OPERATION_DML,
         SQLITE_CONSTRAINT_UNIQUE, VPS_ERROR_CLASS_CONSTRAINT, 0, 0},
        {"foreign_key", "23503", VPS_ERROR_OPERATION_DML,
         SQLITE_CONSTRAINT_FOREIGNKEY, VPS_ERROR_CLASS_CONSTRAINT, 0, 0},
        {"not_null", "23502", VPS_ERROR_OPERATION_DML,
         SQLITE_CONSTRAINT_NOTNULL, VPS_ERROR_CLASS_CONSTRAINT, 0, 0},
        {"check", "23514", VPS_ERROR_OPERATION_DML,
         SQLITE_CONSTRAINT_CHECK, VPS_ERROR_CLASS_CONSTRAINT, 0, 0},
        {"serialization", "40001", VPS_ERROR_OPERATION_DML, SQLITE_BUSY,
         VPS_ERROR_CLASS_SERIALIZATION, 1, 0},
        {"deadlock", "40P01", VPS_ERROR_OPERATION_DML, SQLITE_BUSY,
         VPS_ERROR_CLASS_DEADLOCK, 1, 0},
        {"lock", "55P03", VPS_ERROR_OPERATION_DML, SQLITE_BUSY,
         VPS_ERROR_CLASS_LOCK, 1, 0},
        {"cancel", "57014", VPS_ERROR_OPERATION_QUERY, SQLITE_INTERRUPT,
         VPS_ERROR_CLASS_CANCEL, 0, 0},
        {"password", "28P01", VPS_ERROR_OPERATION_CONNECT, SQLITE_AUTH,
         VPS_ERROR_CLASS_AUTH, 0, 0},
        {"connect_privilege", "42501", VPS_ERROR_OPERATION_CONNECT,
         SQLITE_AUTH, VPS_ERROR_CLASS_AUTH, 0, 0},
        {"query_privilege", "42501", VPS_ERROR_OPERATION_QUERY,
         SQLITE_PERM, VPS_ERROR_CLASS_AUTH, 0, 0},
        {"metadata_table", "42P01", VPS_ERROR_OPERATION_METADATA,
         SQLITE_SCHEMA, VPS_ERROR_CLASS_SCHEMA, 0, 0},
        {"scan_column", "42703", VPS_ERROR_OPERATION_SCAN, SQLITE_SCHEMA,
         VPS_ERROR_CLASS_SCHEMA, 0, 0},
        {"query_table", "42P01", VPS_ERROR_OPERATION_QUERY, SQLITE_ERROR,
         VPS_ERROR_CLASS_SQL, 0, 0},
        {"connect_loss", "08006", VPS_ERROR_OPERATION_CONNECT,
         SQLITE_CANTOPEN, VPS_ERROR_CLASS_CONNECTION, 1, 0},
        {"scan_loss", "08006", VPS_ERROR_OPERATION_SCAN, SQLITE_IOERR,
         VPS_ERROR_CLASS_CONNECTION, 1, 0},
        {"commit_loss", "08006", VPS_ERROR_OPERATION_COMMIT, SQLITE_IOERR,
         VPS_ERROR_CLASS_CONNECTION, 1, 1},
        {"unknown", "ZZ999", VPS_ERROR_OPERATION_QUERY, SQLITE_ERROR,
         VPS_ERROR_CLASS_SQL, 0, 0}};
    VpsErrorAllocatorState state = {0U, 0U};
    VpsAllocator allocator;
    VpsError error = {0};
    size_t index;
    int passed = 1;

    passed &= vps_error_expect(
        vps_allocator_init(&allocator, VPS_ALLOCATOR_FAMILY_SQLITE, &state,
                           vps_error_test_allocate,
                           vps_error_test_reallocate,
                           vps_error_test_deallocate) == VPS_MEMORY_OK &&
            vps_error_init(&error, &allocator) == VPS_MEMORY_OK,
        "sqlstate_init");
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const VpsSqlstateCase *test_case = &cases[index];
        VpsMemoryResult result = vps_error_set_sqlstate(
            &error, test_case->operation, test_case->sqlstate, 7, 2, NULL);
        passed &= vps_error_expect(
            result == VPS_MEMORY_OK &&
                error.sqlite_code == test_case->sqlite_code &&
                error.error_class == test_case->error_class &&
                error.transient == test_case->transient &&
                error.ambiguous == test_case->ambiguous &&
                strcmp(error.sqlstate, test_case->sqlstate) == 0 &&
                error.backend_status == 7 && error.severity_class == 2 &&
                vps_error_message(&error)[0] != '\0',
            test_case->name);
    }
    vps_error_reset(&error);
    vps_error_reset(&error);
    passed &= vps_error_expect(state.allocations == state.deallocations,
                               "sqlstate_cleanup_balanced");
    return passed;
}

static int vps_error_test_local_and_validation(void)
{
    static const char fingerprint[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    VpsErrorAllocatorState state = {0U, 0U};
    VpsAllocator allocator;
    VpsError error = {0};
    int preserved_code;
    int passed = 1;

    passed &= vps_error_expect(
        vps_allocator_init(&allocator, VPS_ALLOCATOR_FAMILY_TEST, &state,
                           vps_error_test_allocate,
                           vps_error_test_reallocate,
                           vps_error_test_deallocate) == VPS_MEMORY_OK &&
            vps_error_init(&error, &allocator) == VPS_MEMORY_OK,
        "local_init");
    passed &= vps_error_expect(
        vps_error_set_local(&error, VPS_ERROR_OPERATION_POOL_WAIT,
                            VPS_ERROR_CLASS_POOL, fingerprint) ==
                VPS_MEMORY_OK &&
            error.sqlite_code == SQLITE_BUSY &&
            strcmp(error.query_fingerprint, fingerprint) == 0,
        "local_pool_busy");
    passed &= vps_error_expect(
        vps_error_set_local(&error, VPS_ERROR_OPERATION_QUERY,
                            VPS_ERROR_CLASS_MEMORY, NULL) == VPS_MEMORY_OK &&
            error.sqlite_code == SQLITE_NOMEM,
        "local_out_of_memory");
    passed &= vps_error_expect(
        vps_error_set_local(&error, VPS_ERROR_OPERATION_CANCEL,
                            VPS_ERROR_CLASS_CANCEL, NULL) == VPS_MEMORY_OK &&
            error.sqlite_code == SQLITE_INTERRUPT,
        "local_cancel");

    preserved_code = error.sqlite_code;
    passed &= vps_error_expect(
        vps_error_set_sqlstate(&error, VPS_ERROR_OPERATION_QUERY, "2350",
                               0, 0, NULL) ==
                VPS_MEMORY_INVALID_ARGUMENT &&
            error.sqlite_code == preserved_code,
        "truncated_sqlstate_rejected");
    passed &= vps_error_expect(
        vps_error_set_sqlstate(&error, VPS_ERROR_OPERATION_QUERY, "23p05",
                               0, 0, NULL) ==
                VPS_MEMORY_INVALID_ARGUMENT &&
            error.sqlite_code == preserved_code,
        "invalid_sqlstate_rejected");
    passed &= vps_error_expect(
        vps_error_set_sqlstate(&error, VPS_ERROR_OPERATION_QUERY, "23505",
                               0, 0, "short") ==
                VPS_MEMORY_INVALID_ARGUMENT &&
            error.sqlite_code == preserved_code,
        "invalid_fingerprint_rejected");
    passed &= vps_error_expect(
        strstr(vps_error_message(&error), "password") == NULL &&
            strstr(vps_error_message(&error), "query") == NULL,
        "canonical_message_redacted");
    vps_error_reset(&error);
    passed &= vps_error_expect(state.allocations == state.deallocations,
                               "local_cleanup_balanced");
    return passed;
}

static int vps_error_test_fault_copy(void)
{
    VpsErrorAllocatorState state = {0U, 0U};
    VpsAllocator backing = {0};
    VpsAllocator fault_allocator = {0};
    VpsFaultAllocator fault = {0};
    VpsError source = {0};
    VpsError destination = {0};
    int passed = 1;

    passed &= vps_error_expect(
        vps_allocator_init(&backing, VPS_ALLOCATOR_FAMILY_SQLITE, &state,
                           vps_error_test_allocate,
                           vps_error_test_reallocate,
                           vps_error_test_deallocate) == VPS_MEMORY_OK &&
            vps_fault_allocator_init(&fault, &backing, 1U) ==
                VPS_MEMORY_OK &&
            vps_fault_allocator_make(&fault, &fault_allocator) ==
                VPS_MEMORY_OK &&
            vps_error_init(&source, &backing) == VPS_MEMORY_OK &&
            vps_error_init(&destination, &fault_allocator) == VPS_MEMORY_OK,
        "copy_init");
    passed &= vps_error_expect(
        vps_error_set_sqlstate(&source, VPS_ERROR_OPERATION_COMMIT, "08006",
                               3, 4, NULL) == VPS_MEMORY_OK,
        "copy_source_set");
    passed &= vps_error_expect(
        vps_error_set_local(&destination, VPS_ERROR_OPERATION_QUERY,
                            VPS_ERROR_CLASS_MEMORY, NULL) == VPS_MEMORY_OK &&
            destination.sqlite_code == SQLITE_NOMEM &&
            strcmp(vps_error_message(&destination), "out of memory") == 0 &&
            fault.attempt_count == 0U,
        "oom_error_is_allocation_free");
    passed &= vps_error_expect(
        vps_error_copy(&destination, &source) == VPS_MEMORY_OUT_OF_MEMORY &&
            destination.error_class == VPS_ERROR_CLASS_MEMORY &&
            destination.sqlite_code == SQLITE_NOMEM &&
            fault.active_allocations == 0U,
        "copy_oom_preserves_destination");
    source.operation = VPS_ERROR_OPERATION_NONE;
    passed &= vps_error_expect(
        vps_error_copy(&destination, &source) ==
                VPS_MEMORY_INVALID_ARGUMENT &&
            destination.error_class == VPS_ERROR_CLASS_MEMORY,
        "copy_invalid_source_rejected");
    source.operation = VPS_ERROR_OPERATION_COMMIT;
    passed &= vps_error_expect(vps_fault_allocator_reset(&fault, 0U) ==
                                   VPS_MEMORY_OK &&
                                   vps_error_copy(&destination, &source) ==
                                       VPS_MEMORY_OK &&
                                   destination.ambiguous &&
                                   destination.sqlite_code == SQLITE_IOERR &&
                                   strcmp(destination.sqlstate, "08006") == 0,
                               "copy_after_fault");
    vps_error_reset(&destination);
    vps_error_reset(&destination);
    vps_error_reset(&source);
    passed &= vps_error_expect(
        fault.active_allocations == 0U && fault.cleanup_errors == 0U &&
            state.allocations == state.deallocations,
        "copy_cleanup_balanced");
    return passed;
}

int main(void)
{
    int passed = 1;

    passed &= vps_error_test_sqlstate_table();
    passed &= vps_error_test_local_and_validation();
    passed &= vps_error_test_fault_copy();
    (void)printf("[error] level=info class=connection sqlite_code=%d "
                 "sqlstate=08006 fingerprint=validated "
                 "redaction=canonical cases=3 status=%s\n",
                 SQLITE_IOERR, passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
