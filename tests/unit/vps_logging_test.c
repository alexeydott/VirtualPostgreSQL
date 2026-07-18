#include "vps_logging.h"

#include <stdio.h>
#include <string.h>

static const char vps_test_sensitive_value[] =
    "password=synthetic_never_emit_marker";

typedef struct VpsLogCapture {
    VpsLogger *logger;
    const VpsLogEvent *reentrant_event;
    size_t calls;
    size_t fields_seen;
    size_t redacted_seen;
    int sensitive_seen;
    int fail;
    int reenter;
} VpsLogCapture;

static int vps_logging_expect(int condition, const char *case_name)
{
    if (!condition) {
        (void)fprintf(stderr,
                      "[logging] level=error case=%s status=failed\n",
                      case_name);
        return 0;
    }
    return 1;
}

static int vps_logging_contains(const char *value,
                                size_t value_length,
                                const char *expected)
{
    size_t expected_length = strlen(expected);
    size_t start;

    if (expected_length > value_length) {
        return 0;
    }
    for (start = 0U; start <= value_length - expected_length; ++start) {
        if (memcmp(value + start, expected, expected_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int vps_logging_capture_sink(void *context,
                                    const VpsLogEvent *event)
{
    VpsLogCapture *capture = (VpsLogCapture *)context;
    size_t index;

    capture->calls += 1U;
    capture->fields_seen += event->field_count;
    capture->redacted_seen += event->redacted_field_count;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->type != VPS_LOG_FIELD_TYPE_STRING) {
            continue;
        }
        if (vps_logging_contains(
                field->value.string_value.data,
                field->value.string_value.length,
                "synthetic_never_emit_marker")) {
            capture->sensitive_seen = 1;
        }
    }
    if (capture->reenter && capture->logger != NULL &&
        capture->reentrant_event != NULL) {
        capture->reenter = 0;
        vps_logger_emit(capture->logger, capture->reentrant_event);
    }
    return capture->fail ? -1 : 0;
}

static int vps_logging_test_levels_and_redaction(void)
{
    VpsLogCapture capture = {0};
    VpsLogger logger;
    VpsLogLevel level;
    int passed = 1;

    passed &= vps_logging_expect(
        vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG,
                        vps_logging_capture_sink, &capture) == VPS_LOG_OK,
        "levels_init");
    capture.logger = &logger;
    for (level = VPS_LOG_LEVEL_DEBUG; level <= VPS_LOG_LEVEL_ERROR;
         level = (VpsLogLevel)(level + 1)) {
        VpsLogEvent event;
        passed &= vps_logging_expect(
            vps_logger_set_level(&logger, level) == VPS_LOG_OK &&
                vps_log_event_init(&event, level) == VPS_LOG_OK &&
                vps_log_event_add_string(
                    &event, VPS_LOG_FIELD_PROFILE,
                    vps_test_sensitive_value,
                    sizeof(vps_test_sensitive_value) - 1U) ==
                    VPS_LOG_REDACTED &&
                event.redacted_field_count == 1U,
            "level_redaction_build");
        vps_logger_emit(&logger, &event);
    }
    passed &= vps_logging_expect(
        capture.calls == 4U && capture.redacted_seen == 4U &&
            !capture.sensitive_seen && logger.emitted_events == 4U,
        "secret_absent_all_levels");

    passed &= vps_logging_expect(
        vps_logger_set_level(&logger, VPS_LOG_LEVEL_ERROR) == VPS_LOG_OK,
        "runtime_level_error");
    {
        VpsLogEvent event;
        passed &= vps_logging_expect(
            vps_log_event_init(&event, VPS_LOG_LEVEL_WARN) == VPS_LOG_OK &&
                vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS,
                                         "filtered", 8U) == VPS_LOG_OK,
            "filtered_event_build");
        vps_logger_emit(&logger, &event);
    }
    passed &= vps_logging_expect(capture.calls == 4U &&
                                     logger.filtered_events == 1U,
                                 "runtime_threshold_filters_lower_level");

    passed &= vps_logging_expect(
        vps_logger_set_level(&logger, VPS_LOG_LEVEL_OFF) == VPS_LOG_OK,
        "runtime_level_off");
    {
        VpsLogEvent event;
        passed &= vps_logging_expect(
            vps_log_event_init(&event, VPS_LOG_LEVEL_ERROR) == VPS_LOG_OK &&
                vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS,
                                         "disabled", 8U) == VPS_LOG_OK,
            "disabled_event_build");
        vps_logger_emit(&logger, &event);
    }
    passed &= vps_logging_expect(capture.calls == 4U &&
                                     logger.filtered_events == 2U,
                                 "disabled_logger_no_sink");
    return passed;
}

static int vps_logging_test_denylist(void)
{
    static const char *const sensitive_values[] = {
        "PaSsWoRd=value", "passwd=value",      "access_token=value",
        "secret=value",   "private_key=value", "connstr=value",
        "conninfo=value", "authorization=value", "bearer=value",
        "certificate=value", "wkt=value",      "wkb=value"};
    size_t index;
    int passed = 1;

    for (index = 0U;
         index < sizeof(sensitive_values) / sizeof(sensitive_values[0]);
         ++index) {
        const char *sensitive_value = sensitive_values[index];
        VpsLogEvent event;
        passed &= vps_logging_expect(
            vps_log_event_init(&event, VPS_LOG_LEVEL_INFO) == VPS_LOG_OK &&
                vps_log_event_add_string(&event, VPS_LOG_FIELD_SERVICE,
                                         sensitive_value,
                                         strlen(sensitive_value)) ==
                    VPS_LOG_REDACTED &&
                event.field_count == 1U &&
                event.fields[0].redacted &&
                event.fields[0].value.string_value.length == 10U &&
                memcmp(event.fields[0].value.string_value.data, "[redacted]",
                       10U) == 0,
            "denylist_value_redacted");
    }
    return passed;
}

static int vps_logging_test_whitelist_and_bounds(void)
{
    static const VpsLogFieldKey string_keys[] = {
        VPS_LOG_FIELD_OPERATION, VPS_LOG_FIELD_PHASE,
        VPS_LOG_FIELD_ERROR_CLASS, VPS_LOG_FIELD_PROFILE,
        VPS_LOG_FIELD_SERVICE, VPS_LOG_FIELD_SNAPSHOT_MODE,
        VPS_LOG_FIELD_ARCHITECTURE, VPS_LOG_FIELD_VERSION,
        VPS_LOG_FIELD_STATUS};
    static const VpsLogFieldKey uint64_keys[] = {
        VPS_LOG_FIELD_DURATION_MS, VPS_LOG_FIELD_ROW_COUNT,
        VPS_LOG_FIELD_BYTE_COUNT, VPS_LOG_FIELD_POOL_ACTIVE,
        VPS_LOG_FIELD_POOL_IDLE, VPS_LOG_FIELD_POOL_WAITING,
        VPS_LOG_FIELD_PARTICIPANT_COUNT};
    static const char fingerprint[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    char oversized[VPS_LOG_MAX_STRING_LENGTH + 1U];
    VpsLogCapture capture = {0};
    VpsLogger logger;
    VpsLogEvent event;
    int passed = 1;

    (void)memset(oversized, 'a', sizeof(oversized));
    passed &= vps_logging_expect(
        vps_logger_init(&logger, VPS_LOG_LEVEL_INFO,
                        vps_logging_capture_sink, &capture) == VPS_LOG_OK &&
            vps_log_event_init(&event, VPS_LOG_LEVEL_INFO) == VPS_LOG_OK,
        "whitelist_init");
    passed &= vps_logging_expect(
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, "scan",
                                 4U) == VPS_LOG_OK &&
            vps_log_event_add_uint64(&event, VPS_LOG_FIELD_DURATION_MS,
                                     17U) == VPS_LOG_OK &&
            vps_log_event_add_string(&event, VPS_LOG_FIELD_SQLSTATE, "08006",
                                     5U) == VPS_LOG_OK &&
            vps_log_event_add_string(&event,
                                     VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                     fingerprint,
                                     sizeof(fingerprint) - 1U) == VPS_LOG_OK,
        "whitelist_fields");
    passed &= vps_logging_expect(
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, "query",
                                 5U) == VPS_LOG_DUPLICATE_FIELD,
        "duplicate_field_rejected");
    passed &= vps_logging_expect(
        vps_log_event_add_string(&event, VPS_LOG_FIELD_DURATION_MS, "17",
                                 2U) == VPS_LOG_INVALID_ARGUMENT,
        "field_type_rejected");
    passed &= vps_logging_expect(
        vps_log_event_add_string(&event, (VpsLogFieldKey)99, "value", 5U) ==
            VPS_LOG_INVALID_ARGUMENT,
        "non_whitelist_key_rejected");
    passed &= vps_logging_expect(
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PROFILE, oversized,
                                 sizeof(oversized)) ==
            VPS_LOG_LIMIT_EXCEEDED,
        "oversized_value_rejected");
    passed &= vps_logging_expect(
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, "raw value",
                                 9U) == VPS_LOG_MALFORMED_VALUE,
        "malformed_value_rejected");
    {
        VpsLogEvent malformed_event;
        passed &= vps_logging_expect(
            vps_log_event_init(&malformed_event, VPS_LOG_LEVEL_INFO) ==
                    VPS_LOG_OK &&
                vps_log_event_add_string(&malformed_event,
                                         VPS_LOG_FIELD_SQLSTATE, "0800",
                                         4U) == VPS_LOG_MALFORMED_VALUE,
            "malformed_sqlstate_rejected");
    }
    vps_logger_emit(&logger, &event);
    passed &= vps_logging_expect(capture.calls == 1U &&
                                     capture.fields_seen == 4U,
                                 "bounded_event_delivered");

    event.field_count = VPS_LOG_MAX_FIELDS + 1U;
    vps_logger_emit(&logger, &event);
    passed &= vps_logging_expect(capture.calls == 1U &&
                                     logger.invalid_events == 1U,
                                 "mutated_event_fails_closed");

    {
        VpsLogEvent connect_event;
        passed &= vps_logging_expect(
            vps_log_event_init(&connect_event, VPS_LOG_LEVEL_DEBUG) ==
                    VPS_LOG_OK &&
                vps_log_event_add_uint64(&connect_event,
                                         VPS_LOG_FIELD_POLL_COUNT, 3U) ==
                    VPS_LOG_OK &&
                vps_log_event_add_uint64(&connect_event,
                                         VPS_LOG_FIELD_WAIT_COUNT, 2U) ==
                    VPS_LOG_OK &&
                vps_log_event_add_uint64(&connect_event,
                                         VPS_LOG_FIELD_PARAMETER_COUNT, 4U) ==
                    VPS_LOG_OK &&
                vps_log_event_add_uint64(
                    &connect_event, VPS_LOG_FIELD_RESULT_FIELD_COUNT, 1U) ==
                    VPS_LOG_OK,
            "connect_count_fields");
        passed &= vps_logging_expect(
            strcmp(vps_log_field_name(VPS_LOG_FIELD_RETRY_ATTEMPT),
                   "retry_attempt") == 0 &&
                strcmp(vps_log_field_name(VPS_LOG_FIELD_BACKOFF_MS),
                       "backoff_ms") == 0,
            "retry_field_names");
    }
    {
        VpsLogEvent capacity_event;
        size_t index;
        passed &= vps_logging_expect(
            vps_log_event_init(&capacity_event, VPS_LOG_LEVEL_INFO) ==
                VPS_LOG_OK,
            "capacity_event_init");
        for (index = 0U;
             index < sizeof(string_keys) / sizeof(string_keys[0]); ++index) {
            passed &= vps_logging_expect(
                vps_log_event_add_string(&capacity_event, string_keys[index],
                                         "safe", 4U) == VPS_LOG_OK,
                "capacity_string_field");
        }
        for (index = 0U;
             index < sizeof(uint64_keys) / sizeof(uint64_keys[0]); ++index) {
            passed &= vps_logging_expect(
                vps_log_event_add_uint64(&capacity_event,
                                         uint64_keys[index], index) ==
                    VPS_LOG_OK,
                "capacity_uint64_field");
        }
        passed &= vps_logging_expect(
            capacity_event.field_count == VPS_LOG_MAX_FIELDS &&
                vps_log_event_add_string(&capacity_event,
                                         VPS_LOG_FIELD_SQLSTATE, "08006",
                                         5U) == VPS_LOG_LIMIT_EXCEEDED,
            "field_capacity_enforced");
    }
    return passed;
}

static int vps_logging_test_sink_isolation(void)
{
    VpsLogCapture capture = {0};
    VpsLogger logger;
    VpsLogEvent event;
    int passed = 1;

    passed &= vps_logging_expect(
        vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG,
                        vps_logging_capture_sink, &capture) == VPS_LOG_OK &&
            vps_log_event_init(&event, VPS_LOG_LEVEL_ERROR) == VPS_LOG_OK &&
            vps_log_event_add_string(&event, VPS_LOG_FIELD_ERROR_CLASS,
                                     "connection", 10U) == VPS_LOG_OK,
        "sink_init");
    capture.logger = &logger;
    capture.reentrant_event = &event;
    capture.fail = 1;
    capture.reenter = 1;
    passed &= vps_logging_expect(
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PROFILE,
                                 vps_test_sensitive_value,
                                 sizeof(vps_test_sensitive_value) - 1U) ==
            VPS_LOG_REDACTED,
        "sink_secret_redacted");
    vps_logger_emit(&logger, &event);
    passed &= vps_logging_expect(
        capture.calls == 1U && logger.emitted_events == 1U &&
            logger.reentrant_events == 1U && logger.sink_failures == 1U &&
            capture.redacted_seen == 1U && !capture.sensitive_seen,
        "failing_reentrant_sink_isolated");
    return passed;
}

static int vps_logging_test_debug_diagnostics(void)
{
    static const char primary[] =
        "duplicate key value \"synthetic-secret\" violates unique constraint \"fixture_uq\"";
    static const char expected_primary[] =
        "duplicate key value ? violates unique constraint ?";
    static const char sql[] = "SELECT $1::pg_catalog.int4";
    static const char sensitive_primary[] =
        "password authentication failed for user \"admin\"";
    char storage[VPS_LOG_MAX_STRING_LENGTH + 1U];
    VpsLogEvent event;
    VpsLogResult sql_result;
    int passed = 1;
    passed &= vps_logging_expect(
        vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) == VPS_LOG_OK &&
            vps_log_event_add_primary_message(
                &event, primary, sizeof(primary) - 1U, storage,
                sizeof(storage)) == VPS_LOG_OK &&
            event.field_count == 1U &&
            event.fields[0].key == VPS_LOG_FIELD_PRIMARY_MESSAGE &&
            event.fields[0].value.string_value.length ==
                sizeof(expected_primary) - 1U &&
            memcmp(event.fields[0].value.string_value.data,
                   expected_primary, sizeof(expected_primary) - 1U) == 0,
        "primary_message_redacted");
    sql_result = vps_log_event_add_debug_sql(
        &event, sql, sizeof(sql) - 1U);
#if defined(VPS_DEBUG)
    passed &= vps_logging_expect(
        sql_result == VPS_LOG_OK && event.field_count == 2U &&
            event.fields[1].key == VPS_LOG_FIELD_SQL_TEXT &&
            event.fields[1].value.string_value.data == sql,
        "debug_sql_enabled");
#else
    passed &= vps_logging_expect(
        sql_result == VPS_LOG_REDACTED && event.field_count == 1U,
        "release_sql_disabled");
#endif
    passed &= vps_logging_expect(
        vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) == VPS_LOG_OK &&
            vps_log_event_add_primary_message(
                &event, sensitive_primary, sizeof(sensitive_primary) - 1U,
                storage, sizeof(storage)) == VPS_LOG_REDACTED &&
            event.fields[0].redacted,
        "sensitive_primary_fully_redacted");
    return passed;
}

static int vps_logging_test_level_parser(void)
{
    static const char *const names[] = {"debug", "info", "warn", "error",
                                        "off"};
    VpsLogLevel level = VPS_LOG_LEVEL_OFF;
    size_t index;
    int passed = 1;

    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        passed &= vps_logging_expect(
            vps_log_level_parse(names[index], &level) == VPS_LOG_OK &&
                level == (VpsLogLevel)index &&
                strcmp(vps_log_level_name(level), names[index]) == 0,
            "runtime_level_parse");
    }
    passed &= vps_logging_expect(
        vps_log_level_parse("trace", &level) == VPS_LOG_INVALID_ARGUMENT &&
            level == VPS_LOG_LEVEL_OFF,
        "unknown_level_rejected");
    return passed;
}

int main(void)
{
    int passed = 1;

    passed &= vps_logging_test_levels_and_redaction();
    passed &= vps_logging_test_denylist();
    passed &= vps_logging_test_whitelist_and_bounds();
    passed &= vps_logging_test_sink_isolation();
    passed &= vps_logging_test_debug_diagnostics();
    passed &= vps_logging_test_level_parser();
    (void)printf("[logging] level=info fields=whitelist,count "
                 "redaction=passed sink_isolation=passed cases=6 status=%s\n",
                 passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
