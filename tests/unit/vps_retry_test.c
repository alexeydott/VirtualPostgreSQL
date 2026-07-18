#include "vps_retry.h"

#include <stdio.h>
#include <string.h>

static int expect(int condition, const char *name)
{
    if (!condition) {
        (void)fprintf(stderr, "retry_case=%s status=failed\n", name);
        return 0;
    }
    return 1;
}

int main(void)
{
    VpsRetryContext context;
    VpsRetryDecision decision;
    VpsErrorClass forbidden[] = {
        VPS_ERROR_CLASS_CONFIG, VPS_ERROR_CLASS_AUTH, VPS_ERROR_CLASS_TLS,
        VPS_ERROR_CLASS_TIMEOUT, VPS_ERROR_CLASS_CANCEL, VPS_ERROR_CLASS_SQL,
        VPS_ERROR_CLASS_SCHEMA, VPS_ERROR_CLASS_CONSTRAINT};
    size_t index;
    int passed = 1;
    (void)memset(&context, 0, sizeof(context));
    context.error_class = VPS_ERROR_CLASS_CONNECTION;
    context.attempt = 1U;
    context.initial_backoff_ms = 100U;
    context.transient = 1;
    context.idempotent_read = 1;
    passed &= expect(vps_retry_decide(&context, &decision) && decision.retry &&
                         decision.backoff_ms == 100U,
                     "safe_first");
    context.attempt = 2U;
    passed &= expect(vps_retry_decide(&context, &decision) && decision.retry &&
                         decision.backoff_ms == 200U,
                     "safe_second");
    context.attempt = VPS_RETRY_MAX_ATTEMPTS;
    passed &= expect(vps_retry_decide(&context, &decision) && !decision.retry,
                     "attempt_limit");
    context.attempt = 1U;
    for (index = 0U; index < sizeof(forbidden) / sizeof(forbidden[0]); ++index) {
        context.error_class = forbidden[index];
        passed &= expect(vps_retry_decide(&context, &decision) &&
                             !decision.retry,
                         "error_class_forbidden");
    }
    context.error_class = VPS_ERROR_CLASS_CONNECTION;
#define VPS_RETRY_FORBID(member, name)                                    \
    do {                                                                  \
        context.member = 1;                                               \
        passed &= expect(vps_retry_decide(&context, &decision) &&         \
                             !decision.retry, name);                      \
        context.member = 0;                                               \
    } while (0)
    VPS_RETRY_FORBID(transaction_active, "transaction");
    VPS_RETRY_FORBID(row_published, "row_published");
    VPS_RETRY_FORBID(snapshot_published, "snapshot_published");
    VPS_RETRY_FORBID(dml, "dml");
    VPS_RETRY_FORBID(ambiguous, "ambiguous");
    VPS_RETRY_FORBID(cancelled, "cancelled");
    context.transient = 0;
    passed &= expect(vps_retry_decide(&context, &decision) && !decision.retry,
                     "not_transient");
    context.transient = 1;
    context.idempotent_read = 0;
    passed &= expect(vps_retry_decide(&context, &decision) && !decision.retry,
                     "not_idempotent");
    context.idempotent_read = 1;
    context.initial_backoff_ms = 800U;
    context.attempt = 2U;
    passed &= expect(vps_retry_decide(&context, &decision) && decision.retry &&
                         decision.backoff_ms == VPS_RETRY_MAX_BACKOFF_MS,
                     "backoff_cap");
    passed &= expect(!vps_retry_decide(NULL, &decision), "null_context");
    (void)printf("retry_policy status=%s\n", passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
