#include "vps_planner.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
    (void)fprintf(stderr, "planner check failed: %s:%d: %s\n", \
                  __FILE__, __LINE__, #expression); return 0; } } while (0)

static int vps_test_round_trip(void)
{
    VpsAllocator allocator;
    VpsPlannerColumn columns[3] = {
        {20U, VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_ORDER | VPS_PLAN_CAP_EXACT},
        {25U, 0U},
        {2950U, VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_ORDER | VPS_PLAN_CAP_EXACT}
    };
    VpsPlannerConstraintInput constraints[3] = {
        {0, VPS_PLAN_OP_EQ, VPS_PLAN_VALUE_INTEGER, 0U, 1, 0, 0},
        {1, VPS_PLAN_OP_EQ, VPS_PLAN_VALUE_TEXT, 1U, 1, 0, 0},
        {-1, VPS_PLAN_OP_LIMIT, VPS_PLAN_VALUE_INTEGER, 2U, 1, 0, 0}
    };
    VpsPlannerOrderInput order = {2U, 1};
    uint16_t key = 0U;
    VpsPlannerRequest request;
    VpsCompiledPlan plan;
    VpsCompiledPlan decoded;
    VpsBuffer encoded;
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    (void)memset(&request, 0, sizeof(request));
    request.source_fingerprint = UINT64_C(0x123456789abcdef0);
    request.columns = columns;
    request.column_count = 3U;
    request.columns_used = UINT64_C(1) << 2U;
    request.constraints = constraints;
    request.constraint_count = 3U;
    request.order_terms = &order;
    request.order_count = 1U;
    request.key_columns = &key;
    request.key_column_count = 1U;
    request.estimated_base_rows = 1000U;
    CHECK(vps_planner_compile(&request, &plan) == VPS_PLANNER_OK);
    CHECK((plan.flags & VPS_PLAN_FLAG_UNIQUE) != 0U);
    CHECK((plan.flags & VPS_PLAN_FLAG_HAS_RECHECK) != 0U);
    CHECK((plan.flags & VPS_PLAN_FLAG_LIMIT_CONSUMED) == 0U);
    CHECK((plan.flags & VPS_PLAN_FLAG_ORDER_CONSUMED) != 0U);
    CHECK(plan.estimated_rows == 1U);
    CHECK(vps_plan_encode(&plan, &allocator, &encoded) == VPS_PLANNER_OK);
    CHECK(vps_plan_decode((const char *)encoded.data, encoded.size,
                          request.source_fingerprint, &decoded) ==
          VPS_PLANNER_OK);
    CHECK(decoded.constraint_count == plan.constraint_count);
    CHECK(decoded.projection_count == plan.projection_count);
    CHECK(decoded.order_count == plan.order_count);
    CHECK(vps_plan_decode((const char *)encoded.data, encoded.size,
                          UINT64_C(9), &decoded) ==
          VPS_PLANNER_FINGERPRINT_MISMATCH);
    vps_buffer_reset(&encoded);
    return 1;
}

static int vps_test_decoder_rejects_malformed(void)
{
    VpsCompiledPlan plan;
    CHECK(vps_plan_decode("", 0U, 0U, &plan) == VPS_PLANNER_INVALID_PLAN);
    CHECK(vps_plan_decode("00", 2U, 0U, &plan) == VPS_PLANNER_INVALID_PLAN);
    CHECK(vps_plan_decode("xyz", 3U, 0U, &plan) == VPS_PLANNER_INVALID_PLAN);
    CHECK(vps_plan_decode("000000000000000000000000000000000000000000000000"
                          "0000000000000000000000000000000000000000", 88U,
                          0U, &plan) == VPS_PLANNER_INVALID_PLAN);
    return 1;
}

static int vps_test_in_and_bounds(void)
{
    VpsPlannerColumn column = {
        20U, VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_ORDER | VPS_PLAN_CAP_EXACT};
    VpsPlannerConstraintInput constraint = {
        0, VPS_PLAN_OP_EQ, VPS_PLAN_VALUE_INTEGER, 0U, 1, 1, 0};
    VpsPlannerRequest request;
    VpsCompiledPlan plan;
    (void)memset(&request, 0, sizeof(request));
    request.source_fingerprint = 1U;
    request.columns = &column;
    request.column_count = 1U;
    request.constraints = &constraint;
    request.constraint_count = 1U;
    CHECK(vps_planner_compile(&request, &plan) == VPS_PLANNER_OK);
    CHECK(plan.constraint_count == 1U);
    CHECK(plan.constraints[0].operation == VPS_PLAN_OP_IN);
    CHECK((plan.constraints[0].flags & VPS_PLAN_CONSTRAINT_IN) != 0U);
    request.column_count = VPS_PLAN_MAX_COLUMNS + 1U;
    CHECK(vps_planner_compile(&request, &plan) ==
          VPS_PLANNER_INVALID_ARGUMENT);
    return 1;
}

static int vps_test_recheck_order_limit_and_cost(void)
{
    VpsPlannerColumn columns[2] = {
        {20U, VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_ORDER | VPS_PLAN_CAP_EXACT},
        {25U, 0U}
    };
    VpsPlannerConstraintInput constraints[2] = {
        {1, VPS_PLAN_OP_EQ, VPS_PLAN_VALUE_TEXT, 0U, 1, 0, 0},
        {-1, VPS_PLAN_OP_LIMIT, VPS_PLAN_VALUE_INTEGER, 1U, 1, 0, 0}
    };
    VpsPlannerOrderInput order = {1U, 0};
    VpsPlannerRequest request;
    VpsCompiledPlan plan;
    (void)memset(&request, 0, sizeof(request));
    request.source_fingerprint = 4U;
    request.columns = columns;
    request.column_count = 2U;
    request.constraints = constraints;
    request.constraint_count = 2U;
    request.order_terms = &order;
    request.order_count = 1U;
    request.estimated_base_rows = 8000U;
    request.relation_pages = 100U;
    request.query_index_prefix = 2U;
    request.query_index_ordinal = 3U;
    CHECK(vps_planner_compile(&request, &plan) == VPS_PLANNER_OK);
    CHECK((plan.flags & VPS_PLAN_FLAG_HAS_RECHECK) != 0U);
    CHECK((plan.flags & VPS_PLAN_FLAG_LIMIT_CONSUMED) == 0U);
    CHECK((plan.flags & VPS_PLAN_FLAG_ORDER_CONSUMED) == 0U);
    CHECK(plan.selected_index_prefix == 2U);
    CHECK(plan.selected_index_ordinal == 3U);
    constraints[0].column = 0;
    constraints[0].value_class = VPS_PLAN_VALUE_INTEGER;
    order.column = 0U;
    CHECK(vps_planner_compile(&request, &plan) == VPS_PLANNER_OK);
    CHECK((plan.flags & VPS_PLAN_FLAG_HAS_RECHECK) == 0U);
    CHECK((plan.flags & VPS_PLAN_FLAG_LIMIT_CONSUMED) != 0U);
    CHECK((plan.flags & VPS_PLAN_FLAG_ORDER_CONSUMED) != 0U);
    CHECK(plan.constraint_count == 2U);
    CHECK(plan.argument_count == 2U);
    CHECK(plan.estimated_cost_milli > plan.estimated_rows);
    return 1;
}

static int vps_test_projection_bit_63(void)
{
    VpsPlannerColumn columns[66];
    VpsPlannerRequest request;
    VpsCompiledPlan plan;
    size_t index;
    (void)memset(columns, 0, sizeof(columns));
    (void)memset(&request, 0, sizeof(request));
    request.source_fingerprint = 5U;
    request.columns = columns;
    request.column_count = 66U;
    request.columns_used = UINT64_C(1) << 63U;
    CHECK(vps_planner_compile(&request, &plan) == VPS_PLANNER_OK);
    CHECK(plan.projection_count == 3U);
    for (index = 0U; index < plan.projection_count; ++index)
        CHECK(plan.projection[index] == index + 63U);
    return 1;
}

int main(void)
{
    int passed = vps_test_round_trip() &&
                 vps_test_decoder_rejects_malformed() &&
                 vps_test_in_and_bounds() &&
                 vps_test_recheck_order_limit_and_cost() &&
                 vps_test_projection_bit_63();
    (void)printf("planner_unit status=%s\n", passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
