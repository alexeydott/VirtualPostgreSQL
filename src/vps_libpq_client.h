#ifndef VPS_LIBPQ_CLIENT_H
#define VPS_LIBPQ_CLIENT_H

#include "vps_client.h"
#include "vps_deadline.h"
#include "vps_identity.h"
#include "vps_session.h"
#include "vps_tls_policy.h"

#include <stdint.h>

#define VPS_LIBPQ_CLIENT_API_VERSION UINT32_C(3)
#define VPS_LIBPQ_CLIENT_MAX_KEYWORDS 20U
#define VPS_LIBPQ_CLIENT_MAX_ACTIVE_POLLS 16U
#define VPS_LIBPQ_CLIENT_MAX_CONNECT_TIMEOUT_MS UINT64_C(600000)
#define VPS_LIBPQ_CLIENT_DEFAULT_CANCEL_TIMEOUT_MS UINT64_C(5000)
#define VPS_LIBPQ_CLIENT_MAX_CANCEL_TIMEOUT_MS UINT64_C(60000)

typedef enum VpsLibpqPollingStatus {
    VPS_LIBPQ_POLL_FAILED = 0,
    VPS_LIBPQ_POLL_READING = 1,
    VPS_LIBPQ_POLL_WRITING = 2,
    VPS_LIBPQ_POLL_OK = 3,
    VPS_LIBPQ_POLL_ACTIVE = 4
} VpsLibpqPollingStatus;

typedef enum VpsLibpqConnectionStatus {
    VPS_LIBPQ_CONNECTION_BAD = 0,
    VPS_LIBPQ_CONNECTION_OK = 1
} VpsLibpqConnectionStatus;

typedef enum VpsLibpqTransactionStatus {
    VPS_LIBPQ_TRANSACTION_IDLE = 0,
    VPS_LIBPQ_TRANSACTION_ACTIVE = 1,
    VPS_LIBPQ_TRANSACTION_INERROR = 2,
    VPS_LIBPQ_TRANSACTION_UNKNOWN = 3
} VpsLibpqTransactionStatus;

typedef enum VpsLibpqPipelineStatus {
    VPS_LIBPQ_PIPELINE_OFF = 0,
    VPS_LIBPQ_PIPELINE_ON = 1,
    VPS_LIBPQ_PIPELINE_UNKNOWN = 2
} VpsLibpqPipelineStatus;

typedef enum VpsLibpqResetMode {
    VPS_LIBPQ_RESET_DISCARD_ALL = 0,
    VPS_LIBPQ_RESET_STRICT = 1
} VpsLibpqResetMode;

typedef enum VpsLibpqResultStatus {
    VPS_LIBPQ_RESULT_EMPTY = 0,
    VPS_LIBPQ_RESULT_COMMAND_OK = 1,
    VPS_LIBPQ_RESULT_TUPLES_OK = 2,
    VPS_LIBPQ_RESULT_SINGLE_TUPLE = 3,
    VPS_LIBPQ_RESULT_FATAL_ERROR = 4,
    VPS_LIBPQ_RESULT_OTHER = 5
} VpsLibpqResultStatus;

/*
 * This injectable table is copied by vps_libpq_client_init. Production uses
 * vps_libpq_client_default_api; tests provide fake handles without exporting
 * PGconn or libpq headers across the adapter boundary.
 */
typedef struct VpsLibpqClientApi {
    uint32_t structure_size;
    uint32_t api_version;
    void *context;
    void *(*connect_start_params)(void *context,
                                  const char *const *keywords,
                                  const char *const *values,
                                  int expand_dbname);
    int (*set_nonblocking)(void *context, void *connection, int enabled);
    VpsLibpqPollingStatus (*connect_poll)(void *context, void *connection);
    intptr_t (*socket_handle)(void *context, const void *connection);
    VpsLibpqConnectionStatus (*connection_status)(
        void *context,
        const void *connection);
    VpsLibpqTransactionStatus (*transaction_status)(
        void *context,
        const void *connection);
    VpsLibpqPipelineStatus (*pipeline_status)(void *context,
                                               const void *connection);
    VpsErrorClass (*connection_error_class)(void *context,
                                             const void *connection);
    VpsTlsResult (*tls_verify)(void *context,
                               void *connection,
                               const VpsTlsPolicy *policy,
                               VpsTlsOutcome *outcome,
                               VpsLogger *logger);
    int (*identity_verify)(void *context,
                           const void *connection,
                           const VpsConnectionIdentity *identity);
    int (*send_prepare)(void *context,
                        void *connection,
                        const char *name,
                        const char *query,
                        int parameter_count,
                        const uint32_t *parameter_types);
    int (*send_describe_prepared)(void *context,
                                  void *connection,
                                  const char *name);
    int (*send_query_prepared)(void *context,
                               void *connection,
                               const char *name,
                               int parameter_count,
                               const char *const *values,
                               const int *lengths,
                               const int *formats,
                               int result_format);
    int (*send_query_params)(void *context,
                             void *connection,
                             const char *query,
                             int parameter_count,
                             const uint32_t *parameter_types,
                             const char *const *values,
                             const int *lengths,
                             const int *formats,
                             int result_format);
    int (*set_single_row_mode)(void *context, void *connection);
    void *(*cancel_create)(void *context, void *connection);
    int (*cancel_start)(void *context, void *cancel_connection);
    VpsLibpqPollingStatus (*cancel_poll)(void *context,
                                         void *cancel_connection);
    intptr_t (*cancel_socket)(void *context,
                              const void *cancel_connection);
    void (*cancel_reset)(void *context, void *cancel_connection);
    void (*cancel_finish)(void *context, void *cancel_connection);
    int (*flush)(void *context, void *connection);
    int (*consume_input)(void *context, void *connection);
    int (*is_busy)(void *context, const void *connection);
    void *(*get_result)(void *context, void *connection);
    VpsLibpqResultStatus (*result_status)(void *context,
                                          const void *result);
    int (*result_parameter_count)(void *context, const void *result);
    uint32_t (*result_parameter_type)(void *context,
                                      const void *result,
                                      int index);
    int (*result_field_count)(void *context, const void *result);
    uint32_t (*result_field_type)(void *context,
                                  const void *result,
                                  int index);
    const char *(*result_field_name)(void *context,
                                     const void *result,
                                     int index);
    int32_t (*result_field_modifier)(void *context,
                                     const void *result,
                                     int index);
    uint32_t (*result_field_relation)(void *context,
                                      const void *result,
                                      int index);
    int32_t (*result_field_attribute)(void *context,
                                      const void *result,
                                      int index);
    int (*result_field_format)(void *context,
                               const void *result,
                               int index);
    int (*result_row_count)(void *context, const void *result);
    int (*result_value_is_null)(void *context,
                                const void *result,
                                int row,
                                int column);
    const void *(*result_value)(void *context,
                                const void *result,
                                int row,
                                int column);
    int (*result_value_length)(void *context,
                               const void *result,
                               int row,
                               int column);
    const char *(*result_sqlstate)(void *context, const void *result);
    void (*clear_result)(void *context, void *result);
    void (*finish)(void *context, void *connection);
    const char *(*result_primary_message)(void *context,
                                          const void *result);
    const char *(*result_command_tuples)(void *context,
                                         const void *result);
} VpsLibpqClientApi;

typedef struct VpsLibpqClientOptions {
    const VpsAllocator *allocator;
    const VpsPlatformOperations *platform_operations;
    const VpsConnectionConfig *connection_config;
    const VpsConnectionIdentity *identity;
    const VpsTlsPolicy *tls_policy;
    const VpsSessionPlan *session_plan;
    VpsInterruptProbe interrupt_probe;
    void *interrupt_context;
    VpsLogger *logger;
    uint64_t connect_timeout_ms;
    uint64_t cancel_timeout_ms;
    uint32_t wait_slice_ms;
    VpsLibpqResetMode reset_mode;
    const VpsLibpqClientApi *api;
} VpsLibpqClientOptions;

/*
 * Stack-owned, caller-serialized adapter. It copies allocator/API dispatch
 * and TLS policy, but borrows platform operations, secure connection config,
 * canonical identity, session plan, interrupt context and logger. All
 * borrowed values must outlive the adapter and every connection created from
 * it. The adapter never copies or logs credential values.
 */
typedef struct VpsLibpqClient {
    VpsAllocator allocator;
    VpsLibpqClientApi api;
    const VpsPlatformOperations *platform_operations;
    const VpsConnectionConfig *connection_config;
    const VpsConnectionIdentity *identity;
    const VpsSessionPlan *session_plan;
    VpsTlsPolicy tls_policy;
    VpsInterruptProbe interrupt_probe;
    void *interrupt_context;
    VpsLogger *logger;
    uint64_t connect_timeout_ms;
    uint64_t cancel_timeout_ms;
    uint32_t wait_slice_ms;
    VpsLibpqResetMode reset_mode;
    uint64_t statement_serial;
    size_t connection_count;
    int initialized;
} VpsLibpqClient;

const VpsLibpqClientApi *vps_libpq_client_default_api(void);
int vps_libpq_client_library_version(void);
VpsClientStatus vps_libpq_client_init(
    VpsLibpqClient *client,
    const VpsLibpqClientOptions *options);
VpsClientStatus vps_libpq_client_make_operations(
    const VpsLibpqClient *client,
    VpsClientOperations *operations);
VpsClientStatus vps_libpq_client_cleanup(VpsLibpqClient *client);

#endif
