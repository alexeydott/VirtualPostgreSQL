#include "vps_planner.h"

#include <string.h>

#define VPS_PLAN_WIRE_MAGIC UINT32_C(0x31535056)
#define VPS_PLAN_WIRE_HEADER_BYTES 46U
#define VPS_PLAN_WIRE_CONSTRAINT_BYTES 16U
#define VPS_PLAN_WIRE_ORDER_BYTES 4U

static uint64_t vps_plan_min_u64(uint64_t left, uint64_t right)
{
    return left < right ? left : right;
}

static void vps_plan_put_u16(unsigned char *destination, uint16_t value)
{
    destination[0] = (unsigned char)(value & UINT16_C(0xff));
    destination[1] = (unsigned char)((value >> 8) & UINT16_C(0xff));
}

static void vps_plan_put_u32(unsigned char *destination, uint32_t value)
{
    size_t index;
    for (index = 0U; index < 4U; ++index)
        destination[index] = (unsigned char)((value >> (index * 8U)) & 0xffU);
}

static void vps_plan_put_u64(unsigned char *destination, uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index)
        destination[index] = (unsigned char)((value >> (index * 8U)) & 0xffU);
}

static uint16_t vps_plan_get_u16(const unsigned char *source)
{
    return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8));
}

static uint32_t vps_plan_get_u32(const unsigned char *source)
{
    uint32_t value = 0U;
    size_t index;
    for (index = 0U; index < 4U; ++index)
        value |= (uint32_t)source[index] << (index * 8U);
    return value;
}

static uint64_t vps_plan_get_u64(const unsigned char *source)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index)
        value |= (uint64_t)source[index] << (index * 8U);
    return value;
}

static int vps_plan_value_exact(const VpsPlannerColumn *column,
                                VpsPlanOperator operation,
                                VpsPlanValueClass value_class)
{
    if (operation == VPS_PLAN_OP_IS_NULL ||
        operation == VPS_PLAN_OP_IS_NOT_NULL) return 1;
    if ((column->capabilities & (VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_EXACT)) !=
        (VPS_PLAN_CAP_EQUALITY | VPS_PLAN_CAP_EXACT)) return 0;
    if (operation != VPS_PLAN_OP_EQ && operation != VPS_PLAN_OP_NE &&
        operation != VPS_PLAN_OP_IN &&
        (column->capabilities & VPS_PLAN_CAP_ORDER) == 0U) return 0;
    if (value_class == VPS_PLAN_VALUE_UNKNOWN) return 0;
    if ((column->capabilities & VPS_PLAN_CAP_BINARY) != 0U)
        return value_class == VPS_PLAN_VALUE_BLOB;
    return value_class == VPS_PLAN_VALUE_INTEGER ||
           value_class == VPS_PLAN_VALUE_TEXT;
}

static int vps_plan_is_key(const VpsPlannerRequest *request, size_t column)
{
    size_t index;
    for (index = 0U; index < request->key_column_count; ++index)
        if (request->key_columns[index] == column) return 1;
    return 0;
}

static int vps_plan_add_projection(VpsCompiledPlan *plan, size_t column)
{
    size_t index;
    if (column > UINT16_MAX || plan->projection_count >= VPS_PLAN_MAX_COLUMNS)
        return 0;
    for (index = 0U; index < plan->projection_count; ++index)
        if (plan->projection[index] == column) return 1;
    plan->projection[plan->projection_count++] = (uint16_t)column;
    return 1;
}

static void vps_plan_log(const VpsPlannerRequest *request,
                         const VpsCompiledPlan *plan)
{
    VpsLogEvent event;
    static const char operation[] = "planner_compile";
    static const char status[] = "passed";
    if (request->logger == NULL ||
        vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 sizeof(status) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_FORMAT_VERSION,
                                 plan->version) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                 plan->source_fingerprint) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_RESULT_FIELD_COUNT,
                                 plan->projection_count) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PARAMETER_COUNT,
                                 plan->argument_count) != VPS_LOG_OK)
        return;
    vps_logger_emit(request->logger, &event);
}

VpsPlannerResult vps_planner_compile(const VpsPlannerRequest *request,
                                     VpsCompiledPlan *plan)
{
    size_t index;
    size_t exact_key_parts = 0U;
    int has_recheck = 0;
    int order_exact = 1;
    uint64_t base_rows;
    if (request == NULL || plan == NULL || request->columns == NULL ||
        request->column_count == 0U ||
        request->column_count > VPS_PLAN_MAX_COLUMNS ||
        request->constraint_count > VPS_PLAN_MAX_CONSTRAINTS ||
        request->order_count > VPS_PLAN_MAX_ORDER_TERMS ||
        request->key_column_count > VPS_PLAN_MAX_CONSTRAINTS ||
        (request->constraint_count != 0U && request->constraints == NULL) ||
        (request->order_count != 0U && request->order_terms == NULL))
        return VPS_PLANNER_INVALID_ARGUMENT;
    (void)memset(plan, 0, sizeof(*plan));
    plan->version = VPS_PLAN_FORMAT_VERSION;
    plan->source_fingerprint = request->source_fingerprint;
    base_rows = request->estimated_base_rows != 0U
                    ? request->estimated_base_rows : UINT64_C(1000000);
    for (index = 0U; index < request->column_count; ++index) {
        int used = index < 63U
                       ? (request->columns_used & (UINT64_C(1) << index)) != 0U
                       : (request->columns_used & (UINT64_C(1) << 63U)) != 0U;
        if ((used || vps_plan_is_key(request, index)) &&
            !vps_plan_add_projection(plan, index))
            return VPS_PLANNER_LIMIT_EXCEEDED;
    }
    if (plan->projection_count == 0U && !vps_plan_add_projection(plan, 0U))
        return VPS_PLANNER_LIMIT_EXCEEDED;
    for (index = 0U; index < request->constraint_count; ++index) {
        const VpsPlannerConstraintInput *input = &request->constraints[index];
        VpsPlanConstraint *output;
        int exact;
        if (!input->usable) continue;
        if (input->operation == VPS_PLAN_OP_LIMIT ||
            input->operation == VPS_PLAN_OP_OFFSET) continue;
        if (input->column < 0 ||
            (size_t)input->column >= request->column_count) continue;
        exact = input->null_safe && input->value_class == VPS_PLAN_VALUE_NULL
                    ? 1
                    : vps_plan_value_exact(&request->columns[input->column],
                                           input->is_in ? VPS_PLAN_OP_IN
                                                        : input->operation,
                                           input->value_class);
        if (!exact) {
            has_recheck = 1;
            continue;
        }
        if (plan->constraint_count >= VPS_PLAN_MAX_CONSTRAINTS)
            return VPS_PLANNER_LIMIT_EXCEEDED;
        output = &plan->constraints[plan->constraint_count++];
        output->column = input->column;
        output->type_oid = request->columns[input->column].type_oid;
        output->source_index = input->source_index;
        output->operation = (uint8_t)(input->is_in ? VPS_PLAN_OP_IN
                                                   : input->operation);
        output->value_class = (uint8_t)input->value_class;
        output->flags = VPS_PLAN_CONSTRAINT_EXACT;
        if (input->is_in) {
            output->flags |= VPS_PLAN_CONSTRAINT_IN | VPS_PLAN_CONSTRAINT_RECHECK;
            has_recheck = 1;
        }
        if (input->null_safe) output->flags |= VPS_PLAN_CONSTRAINT_NULL_SAFE;
        if (vps_plan_is_key(request, (size_t)input->column)) {
            output->flags |= VPS_PLAN_CONSTRAINT_KEY;
            if (!input->is_in && input->operation == VPS_PLAN_OP_EQ)
                ++exact_key_parts;
        }
        if (input->operation != VPS_PLAN_OP_IS_NULL &&
            input->operation != VPS_PLAN_OP_IS_NOT_NULL)
            output->argument_index = ++plan->argument_count;
        if (!vps_plan_add_projection(plan, (size_t)input->column))
            return VPS_PLANNER_LIMIT_EXCEEDED;
    }
    for (index = 0U; index < request->order_count; ++index) {
        const VpsPlannerOrderInput *input = &request->order_terms[index];
        if (input->column >= request->column_count ||
            (request->columns[input->column].capabilities &
             (VPS_PLAN_CAP_ORDER | VPS_PLAN_CAP_EXACT)) !=
                (VPS_PLAN_CAP_ORDER | VPS_PLAN_CAP_EXACT)) {
            order_exact = 0;
            break;
        }
    }
    if (order_exact) {
        for (index = 0U; index < request->order_count; ++index) {
            const VpsPlannerOrderInput *input = &request->order_terms[index];
            VpsPlanOrderTerm *output = &plan->order_terms[plan->order_count++];
            output->column = input->column;
            output->descending = (uint8_t)(input->descending != 0);
            output->nulls_first = (uint8_t)(input->descending == 0);
            if (!vps_plan_add_projection(plan, input->column))
                return VPS_PLANNER_LIMIT_EXCEEDED;
        }
        if (request->order_count != 0U)
            plan->flags |= VPS_PLAN_FLAG_ORDER_CONSUMED;
    } else if (request->order_count != 0U) {
        has_recheck = 1;
        plan->flags |= VPS_PLAN_FLAG_HAS_RECHECK;
    }
    if (has_recheck) plan->flags |= VPS_PLAN_FLAG_HAS_RECHECK;
    for (index = 0U; index < request->constraint_count; ++index) {
        const VpsPlannerConstraintInput *input = &request->constraints[index];
        VpsPlanConstraint *output;
        if (!input->usable ||
            (input->operation != VPS_PLAN_OP_LIMIT &&
             input->operation != VPS_PLAN_OP_OFFSET) || has_recheck)
            continue;
        if (plan->constraint_count >= VPS_PLAN_MAX_CONSTRAINTS)
            return VPS_PLANNER_LIMIT_EXCEEDED;
        output = &plan->constraints[plan->constraint_count++];
        output->column = -1;
        output->source_index = input->source_index;
        output->operation = (uint8_t)input->operation;
        output->value_class = VPS_PLAN_VALUE_INTEGER;
        output->flags = VPS_PLAN_CONSTRAINT_EXACT;
        output->argument_index = ++plan->argument_count;
        plan->flags |= input->operation == VPS_PLAN_OP_LIMIT
                           ? VPS_PLAN_FLAG_LIMIT_CONSUMED
                           : VPS_PLAN_FLAG_OFFSET_CONSUMED;
    }
    if (request->key_column_count != 0U &&
        exact_key_parts == request->key_column_count) {
        plan->flags |= VPS_PLAN_FLAG_UNIQUE;
        plan->estimated_rows = 1U;
    } else {
        uint64_t divisor = UINT64_C(1) + plan->constraint_count * UINT64_C(8);
        if (request->query_index_prefix != 0U) divisor += UINT64_C(16);
        plan->estimated_rows = base_rows / divisor;
        if (plan->estimated_rows == 0U) plan->estimated_rows = 1U;
    }
    plan->selected_index_prefix =
        (uint16_t)vps_plan_min_u64(request->query_index_prefix, UINT16_MAX);
    plan->estimated_cost_milli =
        plan->estimated_rows * UINT64_C(1000) +
        request->relation_pages * UINT64_C(10);
    if (plan->estimated_cost_milli == 0U) plan->estimated_cost_milli = 1U;
    vps_plan_log(request, plan);
    return VPS_PLANNER_OK;
}

static unsigned char vps_plan_hex_digit(unsigned int value)
{
    static const unsigned char digits[] = "0123456789abcdef";
    return digits[value & 15U];
}

static int vps_plan_hex_value(char value, unsigned char *decoded)
{
    if (value >= '0' && value <= '9') *decoded = (unsigned char)(value - '0');
    else if (value >= 'a' && value <= 'f')
        *decoded = (unsigned char)(value - 'a' + 10);
    else if (value >= 'A' && value <= 'F')
        *decoded = (unsigned char)(value - 'A' + 10);
    else return 0;
    return 1;
}

VpsPlannerResult vps_plan_encode(const VpsCompiledPlan *plan,
                                 const VpsAllocator *allocator,
                                 VpsBuffer *encoded)
{
    unsigned char wire[VPS_PLAN_MAX_ENCODED_BYTES / 2U];
    size_t wire_size;
    size_t offset;
    size_t index;
    if (plan == NULL || allocator == NULL || encoded == NULL ||
        plan->version != VPS_PLAN_FORMAT_VERSION ||
        plan->projection_count > VPS_PLAN_MAX_COLUMNS ||
        plan->constraint_count > VPS_PLAN_MAX_CONSTRAINTS ||
        plan->order_count > VPS_PLAN_MAX_ORDER_TERMS)
        return VPS_PLANNER_INVALID_ARGUMENT;
    wire_size = VPS_PLAN_WIRE_HEADER_BYTES + plan->projection_count * 2U +
                plan->constraint_count * VPS_PLAN_WIRE_CONSTRAINT_BYTES +
                plan->order_count * VPS_PLAN_WIRE_ORDER_BYTES;
    if (wire_size * 2U + 1U > VPS_PLAN_MAX_ENCODED_BYTES)
        return VPS_PLANNER_LIMIT_EXCEEDED;
    (void)memset(wire, 0, wire_size);
    vps_plan_put_u32(wire, VPS_PLAN_WIRE_MAGIC);
    vps_plan_put_u32(wire + 4U, plan->version);
    vps_plan_put_u32(wire + 8U, plan->flags);
    vps_plan_put_u64(wire + 12U, plan->source_fingerprint);
    vps_plan_put_u64(wire + 20U, plan->estimated_rows);
    vps_plan_put_u64(wire + 28U, plan->estimated_cost_milli);
    vps_plan_put_u16(wire + 36U, plan->projection_count);
    vps_plan_put_u16(wire + 38U, plan->constraint_count);
    vps_plan_put_u16(wire + 40U, plan->order_count);
    vps_plan_put_u16(wire + 42U, plan->argument_count);
    vps_plan_put_u16(wire + 44U, plan->selected_index_prefix);
    offset = VPS_PLAN_WIRE_HEADER_BYTES;
    for (index = 0U; index < plan->projection_count; ++index, offset += 2U)
        vps_plan_put_u16(wire + offset, plan->projection[index]);
    for (index = 0U; index < plan->constraint_count; ++index) {
        const VpsPlanConstraint *item = &plan->constraints[index];
        vps_plan_put_u32(wire + offset, (uint32_t)item->column);
        vps_plan_put_u32(wire + offset + 4U, item->type_oid);
        vps_plan_put_u16(wire + offset + 8U, item->source_index);
        vps_plan_put_u16(wire + offset + 10U, item->argument_index);
        vps_plan_put_u16(wire + offset + 12U, item->flags);
        wire[offset + 14U] = item->operation;
        wire[offset + 15U] = item->value_class;
        offset += VPS_PLAN_WIRE_CONSTRAINT_BYTES;
    }
    for (index = 0U; index < plan->order_count; ++index) {
        vps_plan_put_u16(wire + offset, plan->order_terms[index].column);
        wire[offset + 2U] = plan->order_terms[index].descending;
        wire[offset + 3U] = plan->order_terms[index].nulls_first;
        offset += VPS_PLAN_WIRE_ORDER_BYTES;
    }
    if (vps_buffer_init(encoded, allocator, VPS_PLAN_MAX_ENCODED_BYTES) !=
        VPS_MEMORY_OK ||
        vps_buffer_reserve(encoded, wire_size * 2U + 1U) != VPS_MEMORY_OK)
        return VPS_PLANNER_OUT_OF_MEMORY;
    for (index = 0U; index < wire_size; ++index) {
        unsigned char pair[2];
        pair[0] = vps_plan_hex_digit(wire[index] >> 4);
        pair[1] = vps_plan_hex_digit(wire[index]);
        if (vps_buffer_append(encoded, pair, 2U) != VPS_MEMORY_OK) {
            vps_buffer_reset(encoded);
            return VPS_PLANNER_OUT_OF_MEMORY;
        }
    }
    return VPS_PLANNER_OK;
}

VpsPlannerResult vps_plan_decode(const char *encoded,
                                 size_t encoded_length,
                                 uint64_t expected_fingerprint,
                                 VpsCompiledPlan *plan)
{
    unsigned char wire[VPS_PLAN_MAX_ENCODED_BYTES / 2U];
    size_t wire_size;
    size_t expected_size;
    size_t offset;
    size_t index;
    if (encoded == NULL || plan == NULL || encoded_length == 0U ||
        encoded_length >= VPS_PLAN_MAX_ENCODED_BYTES ||
        (encoded_length & 1U) != 0U)
        return VPS_PLANNER_INVALID_PLAN;
    wire_size = encoded_length / 2U;
    if (wire_size < VPS_PLAN_WIRE_HEADER_BYTES) return VPS_PLANNER_INVALID_PLAN;
    for (index = 0U; index < wire_size; ++index) {
        unsigned char high;
        unsigned char low;
        if (!vps_plan_hex_value(encoded[index * 2U], &high) ||
            !vps_plan_hex_value(encoded[index * 2U + 1U], &low))
            return VPS_PLANNER_INVALID_PLAN;
        wire[index] = (unsigned char)((high << 4) | low);
    }
    if (vps_plan_get_u32(wire) != VPS_PLAN_WIRE_MAGIC)
        return VPS_PLANNER_INVALID_PLAN;
    (void)memset(plan, 0, sizeof(*plan));
    plan->version = vps_plan_get_u32(wire + 4U);
    if (plan->version != VPS_PLAN_FORMAT_VERSION)
        return VPS_PLANNER_VERSION_MISMATCH;
    plan->flags = vps_plan_get_u32(wire + 8U);
    if ((plan->flags & ~(VPS_PLAN_FLAG_ORDER_CONSUMED |
                         VPS_PLAN_FLAG_UNIQUE |
                         VPS_PLAN_FLAG_HAS_RECHECK |
                         VPS_PLAN_FLAG_LIMIT_CONSUMED |
                         VPS_PLAN_FLAG_OFFSET_CONSUMED)) != 0U)
        return VPS_PLANNER_INVALID_PLAN;
    plan->source_fingerprint = vps_plan_get_u64(wire + 12U);
    if (plan->source_fingerprint != expected_fingerprint)
        return VPS_PLANNER_FINGERPRINT_MISMATCH;
    plan->estimated_rows = vps_plan_get_u64(wire + 20U);
    plan->estimated_cost_milli = vps_plan_get_u64(wire + 28U);
    plan->projection_count = vps_plan_get_u16(wire + 36U);
    plan->constraint_count = vps_plan_get_u16(wire + 38U);
    plan->order_count = vps_plan_get_u16(wire + 40U);
    plan->argument_count = vps_plan_get_u16(wire + 42U);
    plan->selected_index_prefix = vps_plan_get_u16(wire + 44U);
    if (plan->projection_count > VPS_PLAN_MAX_COLUMNS ||
        plan->constraint_count > VPS_PLAN_MAX_CONSTRAINTS ||
        plan->order_count > VPS_PLAN_MAX_ORDER_TERMS ||
        plan->argument_count > VPS_PLAN_MAX_CONSTRAINTS)
        return VPS_PLANNER_LIMIT_EXCEEDED;
    expected_size = VPS_PLAN_WIRE_HEADER_BYTES + plan->projection_count * 2U +
                    plan->constraint_count * VPS_PLAN_WIRE_CONSTRAINT_BYTES +
                    plan->order_count * VPS_PLAN_WIRE_ORDER_BYTES;
    if (expected_size != wire_size) return VPS_PLANNER_INVALID_PLAN;
    offset = VPS_PLAN_WIRE_HEADER_BYTES;
    for (index = 0U; index < plan->projection_count; ++index, offset += 2U) {
        size_t previous;
        plan->projection[index] = vps_plan_get_u16(wire + offset);
        if (plan->projection[index] >= VPS_PLAN_MAX_COLUMNS)
            return VPS_PLANNER_INVALID_PLAN;
        for (previous = 0U; previous < index; ++previous)
            if (plan->projection[previous] == plan->projection[index])
                return VPS_PLANNER_INVALID_PLAN;
    }
    for (index = 0U; index < plan->constraint_count; ++index) {
        VpsPlanConstraint *item = &plan->constraints[index];
        item->column = (int32_t)vps_plan_get_u32(wire + offset);
        item->type_oid = vps_plan_get_u32(wire + offset + 4U);
        item->source_index = vps_plan_get_u16(wire + offset + 8U);
        item->argument_index = vps_plan_get_u16(wire + offset + 10U);
        item->flags = vps_plan_get_u16(wire + offset + 12U);
        item->operation = wire[offset + 14U];
        item->value_class = wire[offset + 15U];
        if ((item->flags & ~(VPS_PLAN_CONSTRAINT_EXACT |
                             VPS_PLAN_CONSTRAINT_RECHECK |
                             VPS_PLAN_CONSTRAINT_IN |
                             VPS_PLAN_CONSTRAINT_KEY |
                             VPS_PLAN_CONSTRAINT_NULL_SAFE)) != 0U ||
            item->operation < VPS_PLAN_OP_EQ ||
            item->operation > VPS_PLAN_OP_OFFSET ||
            item->argument_index > plan->argument_count ||
            (item->column < 0 && item->operation != VPS_PLAN_OP_LIMIT &&
             item->operation != VPS_PLAN_OP_OFFSET) ||
            item->column >= (int32_t)VPS_PLAN_MAX_COLUMNS)
            return VPS_PLANNER_INVALID_PLAN;
        offset += VPS_PLAN_WIRE_CONSTRAINT_BYTES;
    }
    for (index = 0U; index < plan->order_count; ++index) {
        plan->order_terms[index].column = vps_plan_get_u16(wire + offset);
        plan->order_terms[index].descending = wire[offset + 2U];
        plan->order_terms[index].nulls_first = wire[offset + 3U];
        if (plan->order_terms[index].column >= VPS_PLAN_MAX_COLUMNS ||
            plan->order_terms[index].descending > 1U ||
            plan->order_terms[index].nulls_first > 1U)
            return VPS_PLANNER_INVALID_PLAN;
        offset += VPS_PLAN_WIRE_ORDER_BYTES;
    }
    if (plan->estimated_rows == 0U || plan->estimated_cost_milli == 0U ||
        (((plan->flags & VPS_PLAN_FLAG_ORDER_CONSUMED) != 0U) !=
         (plan->order_count != 0U)))
        return VPS_PLANNER_INVALID_PLAN;
    return VPS_PLANNER_OK;
}

const char *vps_planner_result_name(VpsPlannerResult result)
{
    switch (result) {
        case VPS_PLANNER_OK: return "ok";
        case VPS_PLANNER_INVALID_ARGUMENT: return "invalid_argument";
        case VPS_PLANNER_LIMIT_EXCEEDED: return "limit_exceeded";
        case VPS_PLANNER_INVALID_PLAN: return "invalid_plan";
        case VPS_PLANNER_VERSION_MISMATCH: return "version_mismatch";
        case VPS_PLANNER_FINGERPRINT_MISMATCH: return "fingerprint_mismatch";
        case VPS_PLANNER_OUT_OF_MEMORY: return "out_of_memory";
        default: return "unknown";
    }
}
