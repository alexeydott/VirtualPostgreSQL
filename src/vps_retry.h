#ifndef VPS_RETRY_H
#define VPS_RETRY_H

#include "vps_error.h"
#include "vps_logging.h"

#include <stdint.h>

#define VPS_RETRY_MAX_ATTEMPTS UINT32_C(3)
#define VPS_RETRY_MAX_BACKOFF_MS UINT32_C(1000)

typedef struct VpsRetryContext {
    VpsErrorClass error_class;
    uint32_t attempt;
    uint32_t initial_backoff_ms;
    int transient;
    int idempotent_read;
    int transaction_active;
    int row_published;
    int snapshot_published;
    int dml;
    int ambiguous;
    int cancelled;
    VpsLogger *logger;
    const char *query_fingerprint;
} VpsRetryContext;

typedef struct VpsRetryDecision {
    int retry;
    uint32_t backoff_ms;
} VpsRetryDecision;

int vps_retry_decide(const VpsRetryContext *context,
                     VpsRetryDecision *decision);

#endif
