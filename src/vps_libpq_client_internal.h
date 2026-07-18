#ifndef VPS_LIBPQ_CLIENT_INTERNAL_H
#define VPS_LIBPQ_CLIENT_INTERNAL_H

#include "vps_libpq_client.h"

typedef enum VpsLibpqConnectionPhase {
    VPS_LIBPQ_PHASE_CREATED = 0,
    VPS_LIBPQ_PHASE_CONNECTING = 1,
    VPS_LIBPQ_PHASE_HEALTH = 2,
    VPS_LIBPQ_PHASE_READY = 3,
    VPS_LIBPQ_PHASE_FAILED = 4
} VpsLibpqConnectionPhase;

typedef enum VpsLibpqHealthPhase {
    VPS_LIBPQ_HEALTH_IDLE = 0,
    VPS_LIBPQ_HEALTH_SEND = 1,
    VPS_LIBPQ_HEALTH_FLUSH = 2,
    VPS_LIBPQ_HEALTH_RECEIVE = 3
} VpsLibpqHealthPhase;

typedef struct VpsLibpqConnection {
    VpsLibpqClient *client;
    void *postgresql_connection;
    VpsDeadline deadline;
    intptr_t socket_handle;
    VpsClientWaitInterest wait_interest;
    uint64_t poll_count;
    uint64_t wait_count;
    size_t statement_count;
    VpsLibpqConnectionPhase phase;
    VpsLibpqHealthPhase health_phase;
    VpsClientOperation active_operation;
    VpsSessionPhase session_phase;
    size_t session_index;
    int health_result_seen;
    int reset_discard_complete;
    char session_value[VPS_SESSION_SEARCH_PATH_LIMIT + 1U];
    int deadline_started;
} VpsLibpqConnection;

VpsClientStatus vps_libpq_health_begin(VpsLibpqConnection *connection,
                                       VpsClientOperation operation,
                                       VpsSessionPhase session_phase,
                                       VpsError *error);
VpsClientStatus vps_libpq_health_poll(VpsLibpqConnection *connection,
                                      VpsClientPollResult *result,
                                      VpsError *error);
VpsClientStatus vps_libpq_set_error(VpsError *error,
                                    VpsErrorClass error_class);

VpsClientStatus vps_libpq_statement_create(
    void *context,
    void *connection,
    const VpsClientStatementSpec *spec,
    void **statement,
    VpsError *error);
VpsClientStatus vps_libpq_statement_start(
    void *context,
    void *statement,
    VpsClientOperation operation,
    VpsError *error);
VpsClientStatus vps_libpq_statement_poll(
    void *context,
    void *statement,
    VpsClientPollResult *result,
    VpsError *error);
VpsClientStatus vps_libpq_statement_wait(
    void *context,
    void *statement,
    const VpsClientWaitRequest *wait_request,
    VpsError *error);
VpsClientStatus vps_libpq_statement_metadata(
    void *context,
    const void *statement,
    VpsClientStatementMetadata *metadata,
    VpsError *error);
VpsClientStatus vps_libpq_statement_result_field(
    void *context,
    const void *statement,
    size_t field_index,
    VpsClientResultFieldMetadata *field,
    VpsError *error);
VpsClientStatus vps_libpq_statement_row(
    void *context,
    const void *statement,
    size_t *column_count,
    VpsError *error);
VpsClientStatus vps_libpq_statement_column(
    void *context,
    const void *statement,
    size_t column_index,
    VpsClientColumnView *column,
    VpsError *error);
void vps_libpq_statement_row_release(void *context, void *statement);
void vps_libpq_statement_destroy(void *context, void *statement);

#endif
