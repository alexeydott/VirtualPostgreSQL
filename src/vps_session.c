#include "vps_session.h"

#include <stdio.h>
#include <string.h>

static const char *const vps_session_parameter_names[] = {
    "client_encoding",
    "DateStyle",
    "IntervalStyle",
    "TimeZone",
    "standard_conforming_strings",
    "application_name",
    "search_path",
    "statement_timeout",
    "lock_timeout",
    "idle_in_transaction_session_timeout",
    "default_transaction_read_only"};

static int vps_session_text_is_bounded(const char *value,
                                       size_t length,
                                       size_t limit)
{
    size_t index;
    if (value == NULL || length == 0U || length > limit ||
        memchr(value, '\0', length) != NULL) return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte < 0x20U || byte == 0x7fU) return 0;
    }
    return 1;
}

static int vps_session_unquoted_identifier_valid(const char *value,
                                                  size_t length)
{
    size_t index;
    if (length == 0U || length > 63U) return 0;
    if (!((value[0] >= 'A' && value[0] <= 'Z') ||
          (value[0] >= 'a' && value[0] <= 'z') || value[0] == '_')) return 0;
    for (index = 1U; index < length; ++index) {
        char current = value[index];
        if (!((current >= 'A' && current <= 'Z') ||
              (current >= 'a' && current <= 'z') ||
              (current >= '0' && current <= '9') || current == '_' ||
              current == '$')) return 0;
    }
    return 1;
}

static int vps_session_quoted_identifier_valid(const char *value,
                                                size_t length)
{
    size_t index = 1U;
    size_t decoded_length = 0U;
    if (length < 3U || value[0] != '"' || value[length - 1U] != '"') return 0;
    while (index < length - 1U) {
        unsigned char current = (unsigned char)value[index];
        if (current == '"') {
            if (index + 1U >= length - 1U || value[index + 1U] != '"') return 0;
            index += 2U;
        } else {
            if (current < 0x20U || current == 0x7fU) return 0;
            index += 1U;
        }
        decoded_length += 1U;
        if (decoded_length > 63U) return 0;
    }
    return decoded_length != 0U;
}

static int vps_session_search_path_valid(const char *value, size_t length)
{
    size_t begin = 0U;
    if (!vps_session_text_is_bounded(value, length,
                                     VPS_SESSION_SEARCH_PATH_LIMIT)) return 0;
    while (begin < length) {
        size_t end = begin;
        size_t item_begin;
        size_t item_end;
        int quoted = 0;
        while (end < length) {
            if (value[end] == '"') {
                if (quoted && end + 1U < length && value[end + 1U] == '"') {
                    end += 2U;
                    continue;
                }
                quoted = !quoted;
            } else if (value[end] == ',' && !quoted) {
                break;
            }
            end += 1U;
        }
        if (quoted) return 0;
        item_begin = begin;
        item_end = end;
        while (item_begin < item_end &&
               (value[item_begin] == ' ' || value[item_begin] == '\t')) {
            item_begin += 1U;
        }
        while (item_end > item_begin &&
               (value[item_end - 1U] == ' ' || value[item_end - 1U] == '\t')) {
            item_end -= 1U;
        }
        if (item_begin == item_end ||
            !(value[item_begin] == '"'
                  ? vps_session_quoted_identifier_valid(
                        value + item_begin, item_end - item_begin)
                  : vps_session_unquoted_identifier_valid(
                        value + item_begin, item_end - item_begin))) return 0;
        if (end == length) return 1;
        begin = end + 1U;
    }
    return 0;
}

static int vps_session_parse_uint32(const char *value, uint32_t *output)
{
    uint64_t parsed = 0U;
    size_t index;
    if (value == NULL || value[0] == '\0') return 0;
    for (index = 0U; value[index] != '\0'; ++index) {
        unsigned char current = (unsigned char)value[index];
        if (current < '0' || current > '9') return 0;
        parsed = parsed * 10U + (uint64_t)(current - '0');
        if (parsed > UINT32_MAX) return 0;
    }
    *output = (uint32_t)parsed;
    return 1;
}

static uint32_t vps_session_timeout(const VpsParsedArguments *arguments,
                                    VpsArgumentId argument_id,
                                    const char *configured,
                                    int *valid)
{
    const VpsArgumentValue *argument = vps_arguments_get(arguments, argument_id);
    uint32_t value = 0U;
    if (argument != NULL && argument->present) return argument->uint32_value;
    if (configured != NULL && !vps_session_parse_uint32(configured, &value)) {
        *valid = 0;
    }
    return value;
}

static VpsSessionResult vps_session_add(VpsSessionPlan *plan,
                                        VpsSessionParameter parameter,
                                        VpsSessionExpectedClass expected,
                                        const char *value,
                                        size_t length)
{
    VpsSessionSetting *setting;
    if (plan->setting_count >= VPS_SESSION_SETTING_COUNT ||
        length > VPS_SESSION_STORAGE_LIMIT - plan->storage_size) {
        return VPS_SESSION_LIMIT_EXCEEDED;
    }
    setting = &plan->settings[plan->setting_count];
    setting->parameter = parameter;
    setting->expected_class = expected;
    setting->value_offset = plan->storage_size;
    setting->value_length = length;
    if (length != 0U) {
        (void)memcpy(plan->storage + plan->storage_size, value, length);
    }
    plan->storage_size += length;
    plan->setting_count += 1U;
    return VPS_SESSION_OK;
}

static VpsSessionResult vps_session_add_timeout(VpsSessionPlan *plan,
                                                VpsSessionParameter parameter,
                                                uint32_t milliseconds)
{
    char text[11];
    int length = snprintf(text, sizeof(text), "%lu",
                          (unsigned long)milliseconds);
    if (length <= 0 || (size_t)length >= sizeof(text)) {
        return VPS_SESSION_INVALID_VALUE;
    }
    return vps_session_add(plan, parameter, VPS_SESSION_EXPECTED_TIMEOUT_MS,
                           text, (size_t)length);
}

static void vps_session_log(const VpsSessionPlan *plan,
                            VpsSessionPhase phase,
                            const VpsSessionSetting *setting,
                            VpsSessionResult result)
{
    static const char operation[] = "session_baseline";
    VpsLogEvent event;
    const char *phase_name;
    const char *status;
    if (plan == NULL || plan->logger == NULL) return;
    phase_name = vps_session_phase_name(phase);
    status = vps_session_result_name(result);
    if (vps_log_event_init(&event, result == VPS_SESSION_OK
                                       ? VPS_LOG_LEVEL_DEBUG
                                       : VPS_LOG_LEVEL_WARN) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase_name,
                                 strlen(phase_name)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) return;
    if (setting != NULL) {
        const char *parameter = vps_session_parameter_name(setting->parameter);
        const char *expected =
            vps_session_expected_class_name(setting->expected_class);
        (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_PARAMETER,
                                       parameter, strlen(parameter));
        (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_EXPECTED_CLASS,
                                       expected, strlen(expected));
    }
    vps_logger_emit(plan->logger, &event);
}

VpsSessionResult vps_session_plan_init(VpsSessionPlan *plan,
                                       VpsLogger *logger)
{
    if (plan == NULL) return VPS_SESSION_INVALID_ARGUMENT;
    (void)memset(plan, 0, sizeof(*plan));
    plan->logger = logger;
    plan->initialized = 1;
    return VPS_SESSION_OK;
}

VpsSessionResult vps_session_plan_build(
    VpsSessionPlan *plan,
    const VpsConnectionConfig *connection,
    const VpsParsedArguments *arguments,
    const VpsSessionBuildOptions *options)
{
    static const char client_encoding[] = "UTF8";
    static const char date_style[] = "ISO, YMD";
    static const char interval_style[] = "iso_8601";
    static const char standard_strings[] = "on";
    static const char read_only[] = "on";
    static const char read_write[] = "off";
    VpsSessionPlan replacement;
    const VpsCredentialConfig *config;
    const VpsArgumentValue *mode;
    const char *timezone;
    const char *application_name;
    const char *search_path;
    size_t timezone_length;
    size_t application_name_length;
    size_t search_path_length;
    uint32_t statement_timeout;
    uint32_t lock_timeout;
    int valid = 1;
#define VPS_SESSION_ADD(expression) \
    do { VpsSessionResult add_result = (expression); \
         if (add_result != VPS_SESSION_OK) return add_result; } while (0)
    if (plan == NULL || !plan->initialized || connection == NULL ||
        !connection->initialized || arguments == NULL ||
        !arguments->initialized || options == NULL) {
        return VPS_SESSION_INVALID_ARGUMENT;
    }
    (void)vps_session_plan_init(&replacement, plan->logger);
    config = &connection->config;
    timezone = options->timezone == NULL ? VPS_SESSION_DEFAULT_TIMEZONE
                                         : options->timezone;
    timezone_length = options->timezone == NULL
                          ? sizeof(VPS_SESSION_DEFAULT_TIMEZONE) - 1U
                          : options->timezone_length;
    application_name = config->application_name == NULL
                           ? VPS_SESSION_DEFAULT_APPLICATION_NAME
                           : config->application_name;
    application_name_length = strlen(application_name);
    search_path = config->search_path == NULL ? VPS_SESSION_DEFAULT_SEARCH_PATH
                                               : config->search_path;
    search_path_length = strlen(search_path);
    if (!vps_session_text_is_bounded(timezone, timezone_length,
                                     VPS_SESSION_TIMEZONE_LIMIT) ||
        !vps_session_text_is_bounded(application_name,
                                     application_name_length,
                                     VPS_SESSION_APPLICATION_NAME_LIMIT) ||
        !vps_session_search_path_valid(search_path, search_path_length)) {
        return VPS_SESSION_INVALID_VALUE;
    }
    statement_timeout = vps_session_timeout(
        arguments, VPS_ARGUMENT_ID_STATEMENT_TIMEOUT,
        config->statement_timeout, &valid);
    lock_timeout = vps_session_timeout(arguments, VPS_ARGUMENT_ID_LOCK_TIMEOUT,
                                       config->lock_timeout, &valid);
    if (!valid) return VPS_SESSION_INVALID_VALUE;
    mode = vps_arguments_get(arguments, VPS_ARGUMENT_ID_MODE);
    VPS_SESSION_ADD(vps_session_add(&replacement,
        VPS_SESSION_PARAMETER_CLIENT_ENCODING, VPS_SESSION_EXPECTED_EXACT,
        client_encoding, sizeof(client_encoding) - 1U));
    VPS_SESSION_ADD(vps_session_add(&replacement, VPS_SESSION_PARAMETER_DATESTYLE,
        VPS_SESSION_EXPECTED_EXACT, date_style, sizeof(date_style) - 1U));
    VPS_SESSION_ADD(vps_session_add(&replacement,
        VPS_SESSION_PARAMETER_INTERVALSTYLE, VPS_SESSION_EXPECTED_EXACT,
        interval_style, sizeof(interval_style) - 1U));
    VPS_SESSION_ADD(vps_session_add(&replacement, VPS_SESSION_PARAMETER_TIMEZONE,
        VPS_SESSION_EXPECTED_EXACT, timezone, timezone_length));
    VPS_SESSION_ADD(vps_session_add(&replacement,
        VPS_SESSION_PARAMETER_STANDARD_STRINGS, VPS_SESSION_EXPECTED_EXACT,
        standard_strings, sizeof(standard_strings) - 1U));
    VPS_SESSION_ADD(vps_session_add(&replacement,
        VPS_SESSION_PARAMETER_APPLICATION_NAME, VPS_SESSION_EXPECTED_EXACT,
        application_name, application_name_length));
    VPS_SESSION_ADD(vps_session_add(&replacement,
        VPS_SESSION_PARAMETER_SEARCH_PATH, VPS_SESSION_EXPECTED_EXACT,
        search_path, search_path_length));
    VPS_SESSION_ADD(vps_session_add_timeout(&replacement,
        VPS_SESSION_PARAMETER_STATEMENT_TIMEOUT, statement_timeout));
    VPS_SESSION_ADD(vps_session_add_timeout(&replacement,
        VPS_SESSION_PARAMETER_LOCK_TIMEOUT, lock_timeout));
    VPS_SESSION_ADD(vps_session_add_timeout(&replacement,
        VPS_SESSION_PARAMETER_IDLE_TRANSACTION_TIMEOUT,
        options->idle_in_transaction_timeout_ms));
    VPS_SESSION_ADD(vps_session_add(&replacement,
        VPS_SESSION_PARAMETER_DEFAULT_READ_ONLY, VPS_SESSION_EXPECTED_EXACT,
        mode != NULL && mode->present &&
                mode->enum_value == VPS_ARGUMENT_ENUM_MODE_RW
            ? read_write : read_only,
        mode != NULL && mode->present &&
                mode->enum_value == VPS_ARGUMENT_ENUM_MODE_RW
            ? sizeof(read_write) - 1U : sizeof(read_only) - 1U));
    replacement.built = 1;
    *plan = replacement;
    return VPS_SESSION_OK;
#undef VPS_SESSION_ADD
}

void vps_session_plan_reset(VpsSessionPlan *plan)
{
    VpsLogger *logger;
    if (plan == NULL) return;
    logger = plan->logger;
    (void)memset(plan, 0, sizeof(*plan));
    plan->logger = logger;
    plan->initialized = 1;
}

const VpsSessionSetting *vps_session_setting_at(const VpsSessionPlan *plan,
                                                size_t index)
{
    if (plan == NULL || !plan->built || index >= plan->setting_count) return NULL;
    return &plan->settings[index];
}

const char *vps_session_setting_value(const VpsSessionPlan *plan,
                                      const VpsSessionSetting *setting)
{
    if (plan == NULL || !plan->built || setting == NULL ||
        setting->value_offset > plan->storage_size ||
        setting->value_length > plan->storage_size - setting->value_offset) {
        return NULL;
    }
    return (const char *)(plan->storage + setting->value_offset);
}

static int vps_session_observed_timeout(const char *value,
                                        size_t length,
                                        uint64_t *milliseconds)
{
    uint64_t parsed = 0U;
    uint64_t multiplier = 1U;
    size_t index = 0U;
    if (value == NULL || length == 0U) return 0;
    while (index < length && value[index] >= '0' && value[index] <= '9') {
        parsed = parsed * 10U + (uint64_t)(value[index] - '0');
        if (parsed > UINT32_MAX) return 0;
        index += 1U;
    }
    if (index == 0U) return 0;
    if (index < length) {
        const char *unit = value + index;
        size_t unit_length = length - index;
        if (unit_length == 2U && memcmp(unit, "ms", 2U) == 0) {
            /* Milliseconds are the base unit. */
        }
        else if (unit_length == 1U && unit[0] == 's') multiplier = 1000U;
        else if (unit_length == 3U && memcmp(unit, "min", 3U) == 0) multiplier = 60000U;
        else if (unit_length == 1U && unit[0] == 'h') multiplier = 3600000U;
        else if (unit_length == 1U && unit[0] == 'd') multiplier = 86400000U;
        else return 0;
    }
    if (parsed > UINT64_MAX / multiplier) return 0;
    *milliseconds = parsed * multiplier;
    return 1;
}

int vps_session_setting_matches(const VpsSessionPlan *plan,
                                const VpsSessionSetting *setting,
                                const char *observed,
                                size_t observed_length)
{
    const char *expected = vps_session_setting_value(plan, setting);
    if (expected == NULL || observed == NULL) return 0;
    if (setting->expected_class == VPS_SESSION_EXPECTED_EXACT) {
        return observed_length == setting->value_length &&
               memcmp(expected, observed, observed_length) == 0;
    }
    if (setting->expected_class == VPS_SESSION_EXPECTED_TIMEOUT_MS) {
        uint32_t expected_ms;
        uint64_t observed_ms;
        char expected_text[11];
        if (setting->value_length >= sizeof(expected_text)) return 0;
        (void)memcpy(expected_text, expected, setting->value_length);
        expected_text[setting->value_length] = '\0';
        return vps_session_parse_uint32(expected_text, &expected_ms) &&
               vps_session_observed_timeout(observed, observed_length,
                                            &observed_ms) &&
               observed_ms == expected_ms;
    }
    return 0;
}

static int vps_session_state_clean(const VpsSessionConnectionState *state)
{
    return state != NULL && state->transaction_idle &&
           state->pipeline_disabled && state->pending_results_absent;
}

VpsSessionResult vps_session_apply(
    const VpsSessionPlan *plan,
    const VpsSessionClientOperations *operations,
    VpsSessionPhase phase)
{
    VpsSessionConnectionState state;
    size_t index;
    VpsSessionResult result;
    if (plan == NULL || !plan->built || operations == NULL ||
        operations->apply_setting == NULL || operations->inspect == NULL ||
        (phase != VPS_SESSION_PHASE_CONNECT && phase != VPS_SESSION_PHASE_RESET)) {
        return VPS_SESSION_INVALID_ARGUMENT;
    }
    (void)memset(&state, 0, sizeof(state));
    result = operations->inspect(operations->context, &state);
    if (result != VPS_SESSION_OK || !vps_session_state_clean(&state)) {
        result = result == VPS_SESSION_OK ? VPS_SESSION_CONNECTION_DIRTY : result;
        vps_session_log(plan, phase, NULL, result);
        return result;
    }
    for (index = 0U; index < plan->setting_count; ++index) {
        const VpsSessionSetting *setting = &plan->settings[index];
        result = operations->apply_setting(operations->context, plan, setting);
        vps_session_log(plan, phase, setting, result);
        if (result != VPS_SESSION_OK) return result;
    }
    (void)memset(&state, 0, sizeof(state));
    result = operations->inspect(operations->context, &state);
    if (result != VPS_SESSION_OK || !vps_session_state_clean(&state)) {
        result = result == VPS_SESSION_OK ? VPS_SESSION_CONNECTION_DIRTY : result;
        vps_session_log(plan, phase, NULL, result);
        return result;
    }
    vps_session_log(plan, phase, NULL, VPS_SESSION_OK);
    return VPS_SESSION_OK;
}

const char *vps_session_parameter_name(VpsSessionParameter parameter)
{
    if (parameter < VPS_SESSION_PARAMETER_CLIENT_ENCODING ||
        parameter > VPS_SESSION_PARAMETER_DEFAULT_READ_ONLY) return "unknown";
    return vps_session_parameter_names[(size_t)parameter];
}

const char *vps_session_expected_class_name(VpsSessionExpectedClass expected)
{
    switch (expected) {
    case VPS_SESSION_EXPECTED_EXACT: return "exact";
    case VPS_SESSION_EXPECTED_TIMEOUT_MS: return "timeout_ms";
    default: return "unknown";
    }
}

const char *vps_session_phase_name(VpsSessionPhase phase)
{
    switch (phase) {
    case VPS_SESSION_PHASE_CONNECT: return "connect";
    case VPS_SESSION_PHASE_RESET: return "reset";
    default: return "unknown";
    }
}

const char *vps_session_result_name(VpsSessionResult result)
{
    switch (result) {
    case VPS_SESSION_OK: return "ok";
    case VPS_SESSION_INVALID_ARGUMENT: return "invalid_argument";
    case VPS_SESSION_INVALID_VALUE: return "invalid_value";
    case VPS_SESSION_LIMIT_EXCEEDED: return "limit_exceeded";
    case VPS_SESSION_CLIENT_ERROR: return "client_error";
    case VPS_SESSION_OBSERVED_MISMATCH: return "observed_mismatch";
    case VPS_SESSION_CONNECTION_DIRTY: return "connection_dirty";
    default: return "unknown";
    }
}
