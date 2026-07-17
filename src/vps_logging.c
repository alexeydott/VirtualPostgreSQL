#include "vps_logging.h"

#include <limits.h>
#include <string.h>

static const char vps_log_redacted_value[] = "[redacted]";

static int vps_log_level_is_valid(VpsLogLevel level)
{
    return level >= VPS_LOG_LEVEL_DEBUG && level <= VPS_LOG_LEVEL_OFF;
}

static int vps_log_event_level_is_valid(VpsLogLevel level)
{
    return level >= VPS_LOG_LEVEL_DEBUG && level <= VPS_LOG_LEVEL_ERROR;
}

static int vps_log_field_key_is_valid(VpsLogFieldKey key)
{
    return key >= VPS_LOG_FIELD_OPERATION && key <= VPS_LOG_FIELD_SIZE_CLASS;
}

static int vps_log_field_requires_string(VpsLogFieldKey key)
{
    switch (key) {
    case VPS_LOG_FIELD_OPERATION:
    case VPS_LOG_FIELD_PHASE:
    case VPS_LOG_FIELD_SQLSTATE:
    case VPS_LOG_FIELD_ERROR_CLASS:
    case VPS_LOG_FIELD_PROFILE:
    case VPS_LOG_FIELD_SERVICE:
    case VPS_LOG_FIELD_CONNECTION_FINGERPRINT:
    case VPS_LOG_FIELD_QUERY_FINGERPRINT:
    case VPS_LOG_FIELD_SNAPSHOT_MODE:
    case VPS_LOG_FIELD_ARCHITECTURE:
    case VPS_LOG_FIELD_VERSION:
    case VPS_LOG_FIELD_STATUS:
    case VPS_LOG_FIELD_SIZE_CLASS:
        return 1;
    default:
        return 0;
    }
}

static int vps_log_field_requires_uint64(VpsLogFieldKey key)
{
    switch (key) {
    case VPS_LOG_FIELD_DURATION_MS:
    case VPS_LOG_FIELD_ROW_COUNT:
    case VPS_LOG_FIELD_BYTE_COUNT:
    case VPS_LOG_FIELD_POOL_ACTIVE:
    case VPS_LOG_FIELD_POOL_IDLE:
    case VPS_LOG_FIELD_POOL_WAITING:
    case VPS_LOG_FIELD_PARTICIPANT_COUNT:
        return 1;
    default:
        return 0;
    }
}

static char vps_log_ascii_lower(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

static int vps_log_contains_token(const char *value,
                                  size_t value_length,
                                  const char *token)
{
    size_t token_length = strlen(token);
    size_t start;
    size_t offset;

    if (token_length > value_length) {
        return 0;
    }
    for (start = 0U; start <= value_length - token_length; ++start) {
        for (offset = 0U; offset < token_length; ++offset) {
            if (vps_log_ascii_lower(value[start + offset]) != token[offset]) {
                break;
            }
        }
        if (offset == token_length) {
            return 1;
        }
    }
    return 0;
}

static int vps_log_value_is_sensitive(const char *value, size_t value_length)
{
    static const char *const tokens[] = {
        "password", "passwd",       "token",      "secret",
        "privatekey", "private_key", "connstr",    "conninfo",
        "authorization", "bearer",  "certificate", "wkt",
        "wkb"};
    size_t index;

    for (index = 0U; index < sizeof(tokens) / sizeof(tokens[0]); ++index) {
        if (vps_log_contains_token(value, value_length, tokens[index])) {
            return 1;
        }
    }
    return 0;
}

static int vps_log_value_is_safe_token(const char *value,
                                       size_t value_length)
{
    size_t index;

    if (value_length == 0U) {
        return 0;
    }
    for (index = 0U; index < value_length; ++index) {
        char current = value[index];
        if (!((current >= 'a' && current <= 'z') ||
              (current >= 'A' && current <= 'Z') ||
              (current >= '0' && current <= '9') || current == '_' ||
              current == '-' || current == '.' || current == ':' ||
              current == '/' || current == '@' || current == '+')) {
            return 0;
        }
    }
    return 1;
}

static int vps_log_sqlstate_is_valid(const char *value, size_t value_length)
{
    size_t index;

    if (value_length != 5U) {
        return 0;
    }
    for (index = 0U; index < value_length; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'A' && value[index] <= 'Z'))) {
            return 0;
        }
    }
    return 1;
}

static int vps_log_fingerprint_is_valid(const char *value,
                                        size_t value_length)
{
    size_t index;

    if (value_length != 64U) {
        return 0;
    }
    for (index = 0U; index < value_length; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int vps_log_string_matches_key(VpsLogFieldKey key,
                                      const char *value,
                                      size_t value_length,
                                      int redacted)
{
    if (redacted) {
        return value == vps_log_redacted_value &&
               value_length == sizeof(vps_log_redacted_value) - 1U;
    }
    if (key == VPS_LOG_FIELD_SQLSTATE) {
        return vps_log_sqlstate_is_valid(value, value_length);
    }
    if (key == VPS_LOG_FIELD_CONNECTION_FINGERPRINT ||
        key == VPS_LOG_FIELD_QUERY_FINGERPRINT) {
        return vps_log_fingerprint_is_valid(value, value_length);
    }
    return vps_log_value_is_safe_token(value, value_length);
}

static int vps_log_event_has_key(const VpsLogEvent *event,
                                 VpsLogFieldKey key)
{
    size_t index;

    for (index = 0U; index < event->field_count; ++index) {
        if (event->fields[index].key == key) {
            return 1;
        }
    }
    return 0;
}

static int vps_log_event_is_valid(const VpsLogEvent *event)
{
    size_t index;
    size_t redacted_count = 0U;

    if (event == NULL || !vps_log_event_level_is_valid(event->level) ||
        event->field_count > VPS_LOG_MAX_FIELDS) {
        return 0;
    }
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        size_t prior;

        if (!vps_log_field_key_is_valid(field->key)) {
            return 0;
        }
        for (prior = 0U; prior < index; ++prior) {
            if (event->fields[prior].key == field->key) {
                return 0;
            }
        }
        if (field->type == VPS_LOG_FIELD_TYPE_STRING) {
            if (!vps_log_field_requires_string(field->key) ||
                field->value.string_value.data == NULL ||
                field->value.string_value.length >
                    VPS_LOG_MAX_STRING_LENGTH ||
                !vps_log_string_matches_key(
                    field->key, field->value.string_value.data,
                    field->value.string_value.length, field->redacted)) {
                return 0;
            }
            if (field->redacted) {
                redacted_count += 1U;
            }
        } else if (field->type == VPS_LOG_FIELD_TYPE_UINT64) {
            if (!vps_log_field_requires_uint64(field->key) ||
                field->redacted) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return redacted_count == event->redacted_field_count;
}

static void vps_log_increment(uint64_t *counter)
{
    if (*counter != UINT64_MAX) {
        *counter += 1U;
    }
}

VpsLogResult vps_log_event_init(VpsLogEvent *event, VpsLogLevel level)
{
    if (event == NULL || !vps_log_event_level_is_valid(level)) {
        return VPS_LOG_INVALID_ARGUMENT;
    }
    event->level = level;
    event->field_count = 0U;
    event->redacted_field_count = 0U;
    return VPS_LOG_OK;
}

VpsLogResult vps_log_event_add_string(VpsLogEvent *event,
                                      VpsLogFieldKey key,
                                      const char *value,
                                      size_t value_length)
{
    VpsLogField *field;
    int sensitive;

    if (event == NULL || !vps_log_event_level_is_valid(event->level) ||
        !vps_log_field_key_is_valid(key) ||
        !vps_log_field_requires_string(key) || value == NULL) {
        return VPS_LOG_INVALID_ARGUMENT;
    }
    if (value_length > VPS_LOG_MAX_STRING_LENGTH) {
        return VPS_LOG_LIMIT_EXCEEDED;
    }
    if (event->field_count >= VPS_LOG_MAX_FIELDS) {
        return VPS_LOG_LIMIT_EXCEEDED;
    }
    if (vps_log_event_has_key(event, key)) {
        return VPS_LOG_DUPLICATE_FIELD;
    }

    sensitive = vps_log_value_is_sensitive(value, value_length);
    if (!sensitive && !vps_log_string_matches_key(key, value, value_length,
                                                  0)) {
        return VPS_LOG_MALFORMED_VALUE;
    }
    field = &event->fields[event->field_count];
    field->key = key;
    field->type = VPS_LOG_FIELD_TYPE_STRING;
    field->redacted = sensitive;
    if (sensitive) {
        field->value.string_value.data = vps_log_redacted_value;
        field->value.string_value.length =
            sizeof(vps_log_redacted_value) - 1U;
        event->redacted_field_count += 1U;
    } else {
        field->value.string_value.data = value;
        field->value.string_value.length = value_length;
    }
    event->field_count += 1U;
    return sensitive ? VPS_LOG_REDACTED : VPS_LOG_OK;
}

VpsLogResult vps_log_event_add_uint64(VpsLogEvent *event,
                                      VpsLogFieldKey key,
                                      uint64_t value)
{
    VpsLogField *field;

    if (event == NULL || !vps_log_event_level_is_valid(event->level) ||
        !vps_log_field_key_is_valid(key) ||
        !vps_log_field_requires_uint64(key)) {
        return VPS_LOG_INVALID_ARGUMENT;
    }
    if (event->field_count >= VPS_LOG_MAX_FIELDS) {
        return VPS_LOG_LIMIT_EXCEEDED;
    }
    if (vps_log_event_has_key(event, key)) {
        return VPS_LOG_DUPLICATE_FIELD;
    }
    field = &event->fields[event->field_count];
    field->key = key;
    field->type = VPS_LOG_FIELD_TYPE_UINT64;
    field->redacted = 0;
    field->value.uint64_value = value;
    event->field_count += 1U;
    return VPS_LOG_OK;
}

VpsLogResult vps_logger_init(VpsLogger *logger,
                             VpsLogLevel level,
                             VpsLogSink sink,
                             void *sink_context)
{
    if (logger == NULL || !vps_log_level_is_valid(level)) {
        return VPS_LOG_INVALID_ARGUMENT;
    }
    logger->level = level;
    logger->sink = sink;
    logger->sink_context = sink_context;
    logger->in_sink = 0;
    logger->emitted_events = 0U;
    logger->filtered_events = 0U;
    logger->invalid_events = 0U;
    logger->reentrant_events = 0U;
    logger->sink_failures = 0U;
    return VPS_LOG_OK;
}

VpsLogResult vps_logger_set_level(VpsLogger *logger, VpsLogLevel level)
{
    if (logger == NULL || !vps_log_level_is_valid(level)) {
        return VPS_LOG_INVALID_ARGUMENT;
    }
    logger->level = level;
    return VPS_LOG_OK;
}

void vps_logger_emit(VpsLogger *logger, const VpsLogEvent *event)
{
    int sink_result;

    if (logger == NULL) {
        return;
    }
    if (!vps_log_event_is_valid(event)) {
        vps_log_increment(&logger->invalid_events);
        return;
    }
    if (!vps_log_level_is_valid(logger->level)) {
        vps_log_increment(&logger->invalid_events);
        return;
    }
    if (logger->level == VPS_LOG_LEVEL_OFF ||
        event->level < logger->level || logger->sink == NULL) {
        vps_log_increment(&logger->filtered_events);
        return;
    }
    if (logger->in_sink) {
        vps_log_increment(&logger->reentrant_events);
        return;
    }

    logger->in_sink = 1;
    sink_result = logger->sink(logger->sink_context, event);
    logger->in_sink = 0;
    vps_log_increment(&logger->emitted_events);
    if (sink_result != 0) {
        vps_log_increment(&logger->sink_failures);
    }
}

VpsLogResult vps_log_level_parse(const char *value, VpsLogLevel *level)
{
    if (value == NULL || level == NULL) {
        return VPS_LOG_INVALID_ARGUMENT;
    }
    if (strcmp(value, "debug") == 0) {
        *level = VPS_LOG_LEVEL_DEBUG;
    } else if (strcmp(value, "info") == 0) {
        *level = VPS_LOG_LEVEL_INFO;
    } else if (strcmp(value, "warn") == 0) {
        *level = VPS_LOG_LEVEL_WARN;
    } else if (strcmp(value, "error") == 0) {
        *level = VPS_LOG_LEVEL_ERROR;
    } else if (strcmp(value, "off") == 0) {
        *level = VPS_LOG_LEVEL_OFF;
    } else {
        return VPS_LOG_INVALID_ARGUMENT;
    }
    return VPS_LOG_OK;
}

const char *vps_log_level_name(VpsLogLevel level)
{
    static const char *const names[] = {"debug", "info", "warn", "error",
                                        "off"};

    if (!vps_log_level_is_valid(level)) {
        return "unknown";
    }
    return names[(size_t)level];
}

const char *vps_log_field_name(VpsLogFieldKey key)
{
    static const char *const names[] = {
        "operation",      "phase",       "duration_ms",
        "row_count",      "byte_count",  "sqlstate",
        "error_class",    "profile",     "service",
        "connection_fingerprint",          "query_fingerprint",
        "pool_active",    "pool_idle",   "pool_waiting",
        "participant_count",               "snapshot_mode",
        "architecture",   "version",     "status",       "size_class"};

    if (!vps_log_field_key_is_valid(key)) {
        return "unknown";
    }
    return names[(size_t)key];
}
