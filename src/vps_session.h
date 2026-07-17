#ifndef VPS_SESSION_H
#define VPS_SESSION_H

#include "vps_connection_string.h"

#define VPS_SESSION_SETTING_COUNT 11U
#define VPS_SESSION_STORAGE_LIMIT 8192U
#define VPS_SESSION_TIMEZONE_LIMIT 255U
#define VPS_SESSION_APPLICATION_NAME_LIMIT 63U
#define VPS_SESSION_SEARCH_PATH_LIMIT 4096U
#define VPS_SESSION_DEFAULT_APPLICATION_NAME "VirtualPostgreSQL/0.1.0"
#define VPS_SESSION_DEFAULT_SEARCH_PATH "pg_catalog"
#define VPS_SESSION_DEFAULT_TIMEZONE "UTC"

typedef enum VpsSessionParameter {
    VPS_SESSION_PARAMETER_CLIENT_ENCODING = 0,
    VPS_SESSION_PARAMETER_DATESTYLE = 1,
    VPS_SESSION_PARAMETER_INTERVALSTYLE = 2,
    VPS_SESSION_PARAMETER_TIMEZONE = 3,
    VPS_SESSION_PARAMETER_STANDARD_STRINGS = 4,
    VPS_SESSION_PARAMETER_APPLICATION_NAME = 5,
    VPS_SESSION_PARAMETER_SEARCH_PATH = 6,
    VPS_SESSION_PARAMETER_STATEMENT_TIMEOUT = 7,
    VPS_SESSION_PARAMETER_LOCK_TIMEOUT = 8,
    VPS_SESSION_PARAMETER_IDLE_TRANSACTION_TIMEOUT = 9,
    VPS_SESSION_PARAMETER_DEFAULT_READ_ONLY = 10
} VpsSessionParameter;

typedef enum VpsSessionExpectedClass {
    VPS_SESSION_EXPECTED_EXACT = 0,
    VPS_SESSION_EXPECTED_TIMEOUT_MS = 1
} VpsSessionExpectedClass;

typedef enum VpsSessionPhase {
    VPS_SESSION_PHASE_CONNECT = 0,
    VPS_SESSION_PHASE_RESET = 1
} VpsSessionPhase;

typedef enum VpsSessionResult {
    VPS_SESSION_OK = 0,
    VPS_SESSION_INVALID_ARGUMENT = 1,
    VPS_SESSION_INVALID_VALUE = 2,
    VPS_SESSION_LIMIT_EXCEEDED = 3,
    VPS_SESSION_CLIENT_ERROR = 4,
    VPS_SESSION_OBSERVED_MISMATCH = 5,
    VPS_SESSION_CONNECTION_DIRTY = 6
} VpsSessionResult;

typedef struct VpsSessionBuildOptions {
    const char *timezone;
    size_t timezone_length;
    uint32_t idle_in_transaction_timeout_ms;
} VpsSessionBuildOptions;

typedef struct VpsSessionSetting {
    VpsSessionParameter parameter;
    VpsSessionExpectedClass expected_class;
    size_t value_offset;
    size_t value_length;
} VpsSessionSetting;

/*
 * A plan owns bounded immutable setting bytes in storage. It is rebuilt
 * transactionally and is safe to copy because settings use offsets, not
 * pointers. The logger is borrowed and mutation must be serialized.
 */
typedef struct VpsSessionPlan {
    unsigned char storage[VPS_SESSION_STORAGE_LIMIT];
    VpsSessionSetting settings[VPS_SESSION_SETTING_COUNT];
    size_t storage_size;
    size_t setting_count;
    VpsLogger *logger;
    int initialized;
    int built;
} VpsSessionPlan;

typedef struct VpsSessionConnectionState {
    int transaction_idle;
    int pipeline_disabled;
    int pending_results_absent;
} VpsSessionConnectionState;

typedef VpsSessionResult (*VpsSessionApplySettingFunction)(
    void *context,
    const VpsSessionPlan *plan,
    const VpsSessionSetting *setting);
typedef VpsSessionResult (*VpsSessionInspectFunction)(
    void *context,
    VpsSessionConnectionState *state);

typedef struct VpsSessionClientOperations {
    void *context;
    VpsSessionApplySettingFunction apply_setting;
    VpsSessionInspectFunction inspect;
} VpsSessionClientOperations;

VpsSessionResult vps_session_plan_init(VpsSessionPlan *plan,
                                       VpsLogger *logger);
VpsSessionResult vps_session_plan_build(
    VpsSessionPlan *plan,
    const VpsConnectionConfig *connection,
    const VpsParsedArguments *arguments,
    const VpsSessionBuildOptions *options);
void vps_session_plan_reset(VpsSessionPlan *plan);

const VpsSessionSetting *vps_session_setting_at(const VpsSessionPlan *plan,
                                                size_t index);
const char *vps_session_setting_value(const VpsSessionPlan *plan,
                                      const VpsSessionSetting *setting);
int vps_session_setting_matches(const VpsSessionPlan *plan,
                                const VpsSessionSetting *setting,
                                const char *observed,
                                size_t observed_length);
VpsSessionResult vps_session_apply(
    const VpsSessionPlan *plan,
    const VpsSessionClientOperations *operations,
    VpsSessionPhase phase);

const char *vps_session_parameter_name(VpsSessionParameter parameter);
const char *vps_session_expected_class_name(VpsSessionExpectedClass expected);
const char *vps_session_phase_name(VpsSessionPhase phase);
const char *vps_session_result_name(VpsSessionResult result);

#endif
