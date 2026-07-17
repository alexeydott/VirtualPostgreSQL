#ifndef VPS_PROTO_COMMON_H
#define VPS_PROTO_COMMON_H

#include <winsock2.h>
#include <windows.h>
#include <libpq-fe.h>

#include <stddef.h>
#include <stdint.h>

typedef enum vps_wait_interest {
    VPS_WAIT_READ = 1,
    VPS_WAIT_WRITE = 2
} vps_wait_interest;

typedef enum vps_wait_result {
    VPS_WAIT_READY = 0,
    VPS_WAIT_TIMEOUT = 1,
    VPS_WAIT_INTERRUPTED = 2,
    VPS_WAIT_FAILED = 3
} vps_wait_result;

typedef struct vps_deadline {
    ULONGLONG expires_at_ms;
} vps_deadline;

typedef struct vps_connect_config {
    const char *host;
    const char *port;
    const char *user;
    const char *password;
    const char *database;
    const char *sslmode;
} vps_connect_config;

typedef enum vps_connect_result {
    VPS_CONNECT_OK = 0,
    VPS_CONNECT_FAILED = 1,
    VPS_CONNECT_TIMEOUT = 2,
    VPS_CONNECT_INTERRUPTED = 3,
    VPS_CONNECT_INVALID_INPUT = 4
} vps_connect_result;

typedef struct vps_connect_outcome {
    PGconn *connection;
    vps_connect_result result;
    PostgresPollingStatusType last_poll;
    uint64_t duration_ms;
    unsigned int poll_count;
    unsigned int wait_count;
} vps_connect_outcome;

vps_deadline vps_deadline_after(uint32_t timeout_ms);
uint32_t vps_deadline_remaining(const vps_deadline *deadline);
vps_wait_result vps_wait_socket(PGconn *connection,
                                vps_wait_interest interest,
                                const vps_deadline *deadline,
                                HANDLE interrupt_event);
vps_wait_result vps_wait_raw_socket(int socket_value,
                                    vps_wait_interest interest,
                                    const vps_deadline *deadline,
                                    HANDLE interrupt_event);
vps_connect_result vps_connect_async(const vps_connect_config *config,
                                     uint32_t timeout_ms,
                                     HANDLE interrupt_event,
                                     vps_connect_outcome *outcome);
const char *vps_poll_name(PostgresPollingStatusType status);
const char *vps_connect_result_name(vps_connect_result result);
const char *vps_result_status_name(ExecStatusType status);
void vps_log(const char *level, const char *event, const char *fields);
int vps_flush_async(PGconn *connection, const vps_deadline *deadline, HANDLE interrupt_event);
PGresult *vps_get_result_async(PGconn *connection,
                               const vps_deadline *deadline,
                               HANDLE interrupt_event,
                               int *operation_ok);
int vps_load_test_config(vps_connect_config *config);
PGconn *vps_connect_test_server(uint32_t timeout_ms);
const char *vps_sqlstate(PGresult *result);

#endif
