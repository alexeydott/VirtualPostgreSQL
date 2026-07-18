#include "vps_retry.h"

#include <string.h>

static int vps_retry_fingerprint_valid(const char *fingerprint)
{
    size_t index;
    if (fingerprint == NULL) return 0;
    for (index = 0U; index < VPS_ERROR_FINGERPRINT_LENGTH; ++index) {
        const char value = fingerprint[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return 0;
        }
    }
    return fingerprint[VPS_ERROR_FINGERPRINT_LENGTH] == '\0';
}

static void vps_retry_log(const VpsRetryContext *context,
                          const VpsRetryDecision *decision)
{
    static const char operation[] = "retry_policy";
    const char *status = decision->retry ? "allowed" : "forbidden";
    const char *error_class = vps_error_class_name(context->error_class);
    VpsLogEvent event;
    if (context->logger == NULL ||
        vps_log_event_init(&event, decision->retry ? VPS_LOG_LEVEL_INFO
                                                   : VPS_LOG_LEVEL_DEBUG) !=
            VPS_LOG_OK) {
        return;
    }
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                   sizeof(operation) - 1U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                   strlen(status));
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_ERROR_CLASS,
                                   error_class, strlen(error_class));
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_RETRY_ATTEMPT,
                                   context->attempt);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_BACKOFF_MS,
                                   decision->backoff_ms);
    if (vps_retry_fingerprint_valid(context->query_fingerprint)) {
        (void)vps_log_event_add_string(
            &event, VPS_LOG_FIELD_QUERY_FINGERPRINT,
            context->query_fingerprint, VPS_ERROR_FINGERPRINT_LENGTH);
    }
    vps_logger_emit(context->logger, &event);
}

int vps_retry_decide(const VpsRetryContext *context,
                     VpsRetryDecision *decision)
{
    uint64_t backoff;
    uint32_t shift;
    if (context == NULL || decision == NULL || context->attempt == 0U ||
        context->initial_backoff_ms == 0U) {
        return 0;
    }
    decision->retry = 0;
    decision->backoff_ms = 0U;
    if (context->error_class != VPS_ERROR_CLASS_CONNECTION ||
        !context->transient || !context->idempotent_read ||
        context->attempt >= VPS_RETRY_MAX_ATTEMPTS ||
        context->transaction_active || context->row_published ||
        context->snapshot_published || context->dml || context->ambiguous ||
        context->cancelled) {
        vps_retry_log(context, decision);
        return 1;
    }
    shift = context->attempt - 1U;
    backoff = context->initial_backoff_ms;
    while (shift-- != 0U && backoff < VPS_RETRY_MAX_BACKOFF_MS) {
        backoff *= 2U;
    }
    if (backoff > VPS_RETRY_MAX_BACKOFF_MS) {
        backoff = VPS_RETRY_MAX_BACKOFF_MS;
    }
    decision->retry = 1;
    decision->backoff_ms = (uint32_t)backoff;
    vps_retry_log(context, decision);
    return 1;
}
