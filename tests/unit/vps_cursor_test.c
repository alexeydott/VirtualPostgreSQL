#include "vps_cursor.h"

#include <stdio.h>

#define CHECK(expression, label)                                             \
    do {                                                                     \
        if (!(expression)) {                                                 \
            (void)fprintf(stderr, "cursor_case=%s status=failed\n", label); \
            return 0;                                                        \
        }                                                                    \
    } while (0)

static int test_scan_path(void)
{
    VpsCursorMachine machine;
    CHECK(vps_cursor_machine_init(&machine, 7U, NULL) == VPS_CURSOR_OK,
          "init");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_OPEN) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_OPEN,
          "open");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_FILTER) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_FILTERING,
          "filter");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_WAIT) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_WAITING,
          "wait");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_RESUME) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_FILTERING,
          "resume");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_ROW) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_ROW_READY,
          "row");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_FETCH) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_FILTERING,
          "fetch");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_COMPLETE) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_EOF,
          "complete");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_FILTER) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_FILTERING,
          "refilter");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_CANCEL) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_CANCELLING,
          "cancel");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_WAIT) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_WAITING,
          "cancel_wait");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_RESUME) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_CANCELLING,
          "cancel_resume");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_FAIL) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_FAILED,
          "fail");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_CLOSE) ==
              VPS_CURSOR_OK && machine.state == VPS_CURSOR_CLOSED,
          "close");
    return 1;
}

static int test_invalid_transitions(void)
{
    VpsCursorMachine machine;
    CHECK(vps_cursor_machine_init(NULL, 1U, NULL) ==
              VPS_CURSOR_INVALID_ARGUMENT,
          "null_init");
    CHECK(vps_cursor_machine_init(&machine, 0U, NULL) ==
              VPS_CURSOR_INVALID_ARGUMENT,
          "zero_id");
    CHECK(vps_cursor_machine_init(&machine, 1U, NULL) == VPS_CURSOR_OK,
          "valid_init");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_ROW) ==
              VPS_CURSOR_INVALID_TRANSITION && machine.state == VPS_CURSOR_NEW,
          "row_from_new");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_OPEN) ==
              VPS_CURSOR_OK,
          "open_for_invalid");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_RESUME) ==
              VPS_CURSOR_INVALID_TRANSITION && machine.state == VPS_CURSOR_OPEN,
          "resume_without_wait");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_CLOSE) ==
              VPS_CURSOR_OK,
          "close_valid");
    CHECK(vps_cursor_transition(&machine, VPS_CURSOR_EVENT_CLOSE) ==
              VPS_CURSOR_INVALID_TRANSITION,
          "double_close");
    return 1;
}

static int test_limits(void)
{
    VpsCursorLimits limits;
    VpsCursorBudget budget;
    vps_cursor_limits_default(&limits);
    limits.max_result_rows = 2U;
    limits.max_result_bytes = 10U;
    limits.max_column_bytes = 6U;
    limits.max_identity_bytes = 4U;
    limits.max_query_bytes = 8U;
    limits.max_parameter_bytes = 5U;
    limits.max_in_values = 3U;
    CHECK(vps_cursor_budget_init(&budget, &limits, 9U, NULL) ==
              VPS_CURSOR_LIMIT_OK,
          "limit_init");
    CHECK(vps_cursor_budget_check_query(&budget, 8U, 5U, 3U) ==
              VPS_CURSOR_LIMIT_OK,
          "query_exact");
    CHECK(vps_cursor_budget_check_query(&budget, 9U, 5U, 3U) ==
              VPS_CURSOR_LIMIT_EXCEEDED,
          "query_over");
    CHECK(vps_cursor_budget_check_query(&budget, 8U, 6U, 3U) ==
              VPS_CURSOR_LIMIT_EXCEEDED,
          "parameter_over");
    CHECK(vps_cursor_budget_check_query(&budget, 8U, 5U, 4U) ==
              VPS_CURSOR_LIMIT_EXCEEDED,
          "in_over");
    CHECK(vps_cursor_budget_observe_row(&budget, 5U, 5U, 4U) ==
              VPS_CURSOR_LIMIT_OK &&
              vps_cursor_budget_observe_row(&budget, 5U, 6U, 4U) ==
                  VPS_CURSOR_LIMIT_OK,
          "row_exact");
    CHECK(vps_cursor_budget_observe_row(&budget, 0U, 0U, 0U) ==
              VPS_CURSOR_LIMIT_EXCEEDED,
          "row_count_over");
    vps_cursor_budget_reset(&budget);
    CHECK(vps_cursor_budget_observe_row(&budget, 7U, 7U, 1U) ==
              VPS_CURSOR_LIMIT_EXCEEDED,
          "column_over");
    CHECK(vps_cursor_budget_observe_row(&budget, 1U, 1U, 5U) ==
              VPS_CURSOR_LIMIT_EXCEEDED,
          "identity_over");
    CHECK(vps_cursor_budget_observe_row(&budget, 10U, 6U, 4U) ==
              VPS_CURSOR_LIMIT_OK &&
              vps_cursor_budget_observe_row(&budget, 1U, 1U, 1U) ==
                  VPS_CURSOR_LIMIT_EXCEEDED,
          "result_bytes_over");
    vps_cursor_budget_reset(&budget);
    budget.result_bytes = UINT64_MAX;
    CHECK(vps_cursor_budget_observe_row(&budget, 1U, 1U, 1U) ==
              VPS_CURSOR_LIMIT_OVERFLOW,
          "byte_overflow");
    vps_cursor_budget_reset(&budget);
    budget.result_rows = UINT64_MAX;
    CHECK(vps_cursor_budget_observe_row(&budget, 0U, 0U, 0U) ==
              VPS_CURSOR_LIMIT_OVERFLOW,
          "row_overflow");
    vps_cursor_limits_default(&limits);
    CHECK(limits.max_result_bytes ==
              (sizeof(void *) == 4U ? VPS_CURSOR_X86_MAX_RESULT_BYTES
                                    : VPS_CURSOR_X64_MAX_RESULT_BYTES),
          "architecture_default");
    CHECK(limits.max_spatial_points ==
              (sizeof(void *) == 4U ? VPS_CURSOR_X86_MAX_SPATIAL_POINTS
                                    : VPS_CURSOR_X64_MAX_SPATIAL_POINTS),
          "architecture_spatial_default");
    return 1;
}

int main(void)
{
    if (!test_scan_path() || !test_invalid_transitions() || !test_limits())
        return 1;
    (void)fprintf(stdout, "cursor_suite status=passed\n");
    return 0;
}
