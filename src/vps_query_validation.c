#include "vps_query_validation.h"

#include <string.h>

static const char vps_query_wrapper_prefix[] =
    "SELECT * FROM (";
static const char vps_query_wrapper_suffix[] =
    ") AS vps_validation LIMIT 0";

static void vps_query_validation_log(VpsLogger *logger,
                                     const char *phase,
                                     const char *status,
                                     uint64_t field_count,
                                     uint64_t fingerprint,
                                     VpsLogLevel level)
{
    VpsLogEvent event;
    if (logger == NULL || vps_log_event_init(&event, level) != VPS_LOG_OK) {
        return;
    }
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                   "query_validation", 16U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE,
                                   phase, strlen(phase));
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS,
                                   status, strlen(status));
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_FORMAT_VERSION,
                                   VPS_QUERY_WRAPPER_FORMAT_VERSION);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_RESULT_FIELD_COUNT,
                                   field_count);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                   fingerprint);
    vps_logger_emit(logger, &event);
}

VpsQueryValidationResult vps_query_validation_init(
    VpsQueryValidation *validation,
    const VpsAllocator *allocator,
    const char *query,
    size_t query_length,
    uint64_t timeout_ms,
    VpsLogger *logger)
{
    static const unsigned char terminator = 0U;
    size_t wrapper_limit;
    VpsQuerySourceResult scan_result;
    if (validation == NULL || !vps_allocator_is_valid(allocator) ||
        query == NULL || query_length == 0U || timeout_ms == 0U ||
        timeout_ms > VPS_CLIENT_MAX_STATEMENT_TIMEOUT_MS) {
        return VPS_QUERY_VALIDATION_INVALID_ARGUMENT;
    }
    (void)memset(validation, 0, sizeof(*validation));
    validation->logger = logger;
    scan_result = vps_query_source_scan(query, query_length, logger,
                                        &validation->source_analysis);
    if (scan_result != VPS_QUERY_SOURCE_OK) {
        return VPS_QUERY_VALIDATION_SCAN_REJECTED;
    }
    if (vps_size_add(sizeof(vps_query_wrapper_prefix) - 1U,
                     validation->source_analysis.statement_length,
                     &wrapper_limit) != VPS_MEMORY_OK ||
        vps_size_add(wrapper_limit, sizeof(vps_query_wrapper_suffix),
                     &wrapper_limit) != VPS_MEMORY_OK ||
        wrapper_limit > VPS_CLIENT_MAX_QUERY_BYTES +
                            sizeof(vps_query_wrapper_prefix) +
                            sizeof(vps_query_wrapper_suffix)) {
        return VPS_QUERY_VALIDATION_LIMIT_EXCEEDED;
    }
    if (vps_buffer_init(&validation->wrapper, allocator, wrapper_limit) !=
            VPS_MEMORY_OK ||
        vps_buffer_append(&validation->wrapper, vps_query_wrapper_prefix,
                          sizeof(vps_query_wrapper_prefix) - 1U) !=
            VPS_MEMORY_OK ||
        vps_buffer_append(&validation->wrapper, query,
                          validation->source_analysis.statement_length) !=
            VPS_MEMORY_OK ||
        vps_buffer_append(&validation->wrapper, vps_query_wrapper_suffix,
                          sizeof(vps_query_wrapper_suffix) - 1U) !=
            VPS_MEMORY_OK ||
        vps_buffer_append(&validation->wrapper, &terminator, 1U) !=
            VPS_MEMORY_OK) {
        vps_buffer_reset(&validation->wrapper);
        return VPS_QUERY_VALIDATION_OUT_OF_MEMORY;
    }
    validation->statement_spec.query =
        (const char *)validation->wrapper.data;
    validation->statement_spec.query_length = validation->wrapper.size - 1U;
    validation->statement_spec.timeout_ms = timeout_ms;
    validation->statement_spec.prepare = 1;
    validation->statement_spec.single_row = 0;
    validation->statement_spec.discover_result_fields = 1;
    validation->initialized = 1;
    vps_query_validation_log(logger, "wrapper", "ready", 0U,
                             validation->source_analysis.normalized_hash,
                             VPS_LOG_LEVEL_DEBUG);
    return VPS_QUERY_VALIDATION_OK;
}

void vps_query_validation_cleanup(VpsQueryValidation *validation)
{
    if (validation == NULL) return;
    vps_buffer_reset(&validation->wrapper);
    (void)memset(validation, 0, sizeof(*validation));
}

const VpsClientStatementSpec *vps_query_validation_statement_spec(
    const VpsQueryValidation *validation)
{
    return validation != NULL && validation->initialized
               ? &validation->statement_spec : NULL;
}

VpsQueryValidationResult vps_query_describe_result_init(
    VpsQueryDescribeResult *result,
    const VpsAllocator *allocator)
{
    if (result == NULL || !vps_allocator_is_valid(allocator)) {
        return VPS_QUERY_VALIDATION_INVALID_ARGUMENT;
    }
    (void)memset(result, 0, sizeof(*result));
    result->allocator = *allocator;
    result->initialized = 1;
    return VPS_QUERY_VALIDATION_OK;
}

VpsQueryValidationResult vps_query_validation_collect(
    const VpsClientStatement *statement,
    VpsQueryDescribeResult *result,
    VpsError *error)
{
    VpsClientStatementMetadata metadata;
    size_t allocation_size;
    size_t index;
    if (statement == NULL || result == NULL || !result->initialized ||
        result->fields != NULL) return VPS_QUERY_VALIDATION_INVALID_ARGUMENT;
    if (vps_client_statement_metadata(statement, &metadata, error) !=
            VPS_CLIENT_OK) return VPS_QUERY_VALIDATION_CLIENT_ERROR;
    if (!metadata.described || metadata.result_field_count == 0U ||
        metadata.result_field_count > VPS_CLIENT_MAX_RESULT_FIELD_COUNT) {
        return VPS_QUERY_VALIDATION_INVALID_DESCRIPTOR;
    }
    if (vps_size_multiply(metadata.result_field_count,
                          sizeof(*result->fields), &allocation_size) !=
            VPS_MEMORY_OK ||
        vps_memory_allocate(&result->allocator, allocation_size,
                            (void **)&result->fields) != VPS_MEMORY_OK) {
        return VPS_QUERY_VALIDATION_OUT_OF_MEMORY;
    }
    (void)memset(result->fields, 0, allocation_size);
    result->allocation_size = allocation_size;
    result->field_count = metadata.result_field_count;
    result->wrapper_query_fingerprint = metadata.query_fingerprint;
    for (index = 0U; index < result->field_count; ++index) {
        VpsClientResultFieldMetadata field;
        if (vps_client_statement_result_field(statement, index, &field,
                                               error) != VPS_CLIENT_OK ||
            field.name == NULL || field.name_length == 0U ||
            field.name_length > VPS_CLIENT_MAX_FIELD_NAME_BYTES ||
            field.type_oid == 0U) {
            vps_query_describe_result_cleanup(result);
            return VPS_QUERY_VALIDATION_INVALID_DESCRIPTOR;
        }
        (void)memcpy(result->fields[index].name, field.name,
                     field.name_length);
        result->fields[index].name[field.name_length] = '\0';
        result->fields[index].name_length = field.name_length;
        result->fields[index].type_oid = field.type_oid;
        result->fields[index].type_modifier = field.type_modifier;
        result->fields[index].origin_relation_oid =
            field.origin_relation_oid;
        result->fields[index].origin_attribute_number =
            field.origin_attribute_number;
        result->fields[index].format = field.format;
    }
    return VPS_QUERY_VALIDATION_OK;
}

void vps_query_describe_result_cleanup(VpsQueryDescribeResult *result)
{
    if (result == NULL || !result->initialized) return;
    vps_memory_release(&result->allocator, (void **)&result->fields,
                       result->allocation_size);
    result->field_count = 0U;
    result->allocation_size = 0U;
    result->wrapper_query_fingerprint = 0U;
}

const char *vps_query_validation_result_name(VpsQueryValidationResult result)
{
    static const char *const names[] = {
        "ok", "invalid_argument", "scan_rejected", "limit_exceeded",
        "out_of_memory", "client_error", "invalid_descriptor"
    };
    if (result < VPS_QUERY_VALIDATION_OK ||
        result > VPS_QUERY_VALIDATION_INVALID_DESCRIPTOR) return "unknown";
    return names[(size_t)result];
}
