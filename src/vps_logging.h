#ifndef VPS_LOGGING_H
#define VPS_LOGGING_H

#include <stddef.h>
#include <stdint.h>

#define VPS_LOG_MAX_FIELDS 16U
#define VPS_LOG_MAX_STRING_LENGTH 128U
#define VPS_LOG_MAX_SQL_TEXT_LENGTH 4096U

typedef enum VpsLogLevel {
    VPS_LOG_LEVEL_DEBUG = 0,
    VPS_LOG_LEVEL_INFO = 1,
    VPS_LOG_LEVEL_WARN = 2,
    VPS_LOG_LEVEL_ERROR = 3,
    VPS_LOG_LEVEL_OFF = 4
} VpsLogLevel;

typedef enum VpsLogResult {
    VPS_LOG_OK = 0,
    VPS_LOG_REDACTED = 1,
    VPS_LOG_INVALID_ARGUMENT = 2,
    VPS_LOG_LIMIT_EXCEEDED = 3,
    VPS_LOG_DUPLICATE_FIELD = 4,
    VPS_LOG_MALFORMED_VALUE = 5
} VpsLogResult;

typedef enum VpsLogFieldKey {
    VPS_LOG_FIELD_OPERATION = 0,
    VPS_LOG_FIELD_PHASE = 1,
    VPS_LOG_FIELD_DURATION_MS = 2,
    VPS_LOG_FIELD_ROW_COUNT = 3,
    VPS_LOG_FIELD_BYTE_COUNT = 4,
    VPS_LOG_FIELD_SQLSTATE = 5,
    VPS_LOG_FIELD_ERROR_CLASS = 6,
    VPS_LOG_FIELD_PROFILE = 7,
    VPS_LOG_FIELD_SERVICE = 8,
    VPS_LOG_FIELD_CONNECTION_FINGERPRINT = 9,
    VPS_LOG_FIELD_QUERY_FINGERPRINT = 10,
    VPS_LOG_FIELD_POOL_ACTIVE = 11,
    VPS_LOG_FIELD_POOL_IDLE = 12,
    VPS_LOG_FIELD_POOL_WAITING = 13,
    VPS_LOG_FIELD_PARTICIPANT_COUNT = 14,
    VPS_LOG_FIELD_SNAPSHOT_MODE = 15,
    VPS_LOG_FIELD_ARCHITECTURE = 16,
    VPS_LOG_FIELD_VERSION = 17,
    VPS_LOG_FIELD_STATUS = 18,
    VPS_LOG_FIELD_SIZE_CLASS = 19,
    VPS_LOG_FIELD_ARGUMENT = 20,
    VPS_LOG_FIELD_PRESENCE_MASK = 21,
    VPS_LOG_FIELD_PROVIDER_ID = 22,
    VPS_LOG_FIELD_GENERATION = 23,
    VPS_LOG_FIELD_CONFIGURATION_GENERATION = 24,
    VPS_LOG_FIELD_TLS_MODE = 25,
    VPS_LOG_FIELD_SSL_IN_USE = 26,
    VPS_LOG_FIELD_CERTIFICATE_STATUS = 27,
    VPS_LOG_FIELD_CHANNEL_BINDING_STATUS = 28,
    VPS_LOG_FIELD_PARAMETER = 29,
    VPS_LOG_FIELD_EXPECTED_CLASS = 30,
    VPS_LOG_FIELD_POLL_COUNT = 31,
    VPS_LOG_FIELD_WAIT_COUNT = 32,
    VPS_LOG_FIELD_PARAMETER_COUNT = 33,
    VPS_LOG_FIELD_RESULT_FIELD_COUNT = 34,
    VPS_LOG_FIELD_RETRY_ATTEMPT = 35,
    VPS_LOG_FIELD_BACKOFF_MS = 36,
    VPS_LOG_FIELD_PRIMARY_MESSAGE = 37,
    VPS_LOG_FIELD_SQL_TEXT = 38,
    VPS_LOG_FIELD_TYPE_OID = 39,
    VPS_LOG_FIELD_RELATION_OID = 40,
    VPS_LOG_FIELD_NAMESPACE_OID = 41,
    VPS_LOG_FIELD_INDEX_OID = 42,
    VPS_LOG_FIELD_CODEC_ID = 43,
    VPS_LOG_FIELD_KEY_COUNT = 44,
    VPS_LOG_FIELD_FLAGS = 45,
    VPS_LOG_FIELD_CHANGE_CLASS = 46,
    VPS_LOG_FIELD_FORMAT_VERSION = 47
} VpsLogFieldKey;

typedef enum VpsLogFieldType {
    VPS_LOG_FIELD_TYPE_STRING = 1,
    VPS_LOG_FIELD_TYPE_UINT64 = 2
} VpsLogFieldType;

typedef struct VpsLogString {
    const char *data;
    size_t length;
} VpsLogString;

typedef struct VpsLogField {
    VpsLogFieldKey key;
    VpsLogFieldType type;
    int redacted;
    union {
        VpsLogString string_value;
        uint64_t uint64_value;
    } value;
} VpsLogField;

/*
 * Events are stack-friendly, allocation-free views. String storage remains
 * owned by the caller and must stay valid until the synchronous sink returns.
 * Only enum whitelist keys are accepted; there is deliberately no free-form
 * message, SQL, bound-value or spatial-payload field.
 */
typedef struct VpsLogEvent {
    VpsLogLevel level;
    VpsLogField fields[VPS_LOG_MAX_FIELDS];
    size_t field_count;
    size_t redacted_field_count;
} VpsLogEvent;

typedef int (*VpsLogSink)(void *context, const VpsLogEvent *event);

/*
 * Logger does not own sink context. The sink is invoked synchronously and its
 * return value is diagnostic only: failures and recursive calls are counted,
 * never propagated into product control flow. This initial contract is not
 * thread-safe; callers must serialize shared logger mutation and emission.
 */
typedef struct VpsLogger {
    VpsLogLevel level;
    VpsLogSink sink;
    void *sink_context;
    int in_sink;
    uint64_t emitted_events;
    uint64_t filtered_events;
    uint64_t invalid_events;
    uint64_t reentrant_events;
    uint64_t sink_failures;
} VpsLogger;

VpsLogResult vps_log_event_init(VpsLogEvent *event, VpsLogLevel level);
VpsLogResult vps_log_event_add_string(VpsLogEvent *event,
                                      VpsLogFieldKey key,
                                      const char *value,
                                      size_t value_length);
/*
 * Adds a bounded redacted PostgreSQL primary message to a DEBUG event.
 * Quoted spans and non-printable/non-ASCII bytes become '?'; sensitive-token
 * detection can redact the whole field. storage is caller-owned until the
 * synchronous sink returns. DETAIL/HINT/CONTEXT must never be passed here.
 */
VpsLogResult vps_log_event_add_primary_message(
    VpsLogEvent *event,
    const char *value,
    size_t value_length,
    char *storage,
    size_t storage_size);
/* Adds raw SQL only in a VPS_DEBUG build and only to a DEBUG event. */
VpsLogResult vps_log_event_add_debug_sql(VpsLogEvent *event,
                                         const char *sql,
                                         size_t sql_length);
VpsLogResult vps_log_event_add_uint64(VpsLogEvent *event,
                                      VpsLogFieldKey key,
                                      uint64_t value);

VpsLogResult vps_logger_init(VpsLogger *logger,
                             VpsLogLevel level,
                             VpsLogSink sink,
                             void *sink_context);
VpsLogResult vps_logger_set_level(VpsLogger *logger, VpsLogLevel level);
void vps_logger_emit(VpsLogger *logger, const VpsLogEvent *event);

VpsLogResult vps_log_level_parse(const char *value, VpsLogLevel *level);
const char *vps_log_level_name(VpsLogLevel level);
const char *vps_log_field_name(VpsLogFieldKey key);

#endif
