#include "vps_proto_common.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t vps_monotonic_ms(void)
{
    return (uint64_t)GetTickCount64();
}

vps_deadline vps_deadline_after(uint32_t timeout_ms)
{
    vps_deadline deadline;
    deadline.expires_at_ms = GetTickCount64() + (ULONGLONG)timeout_ms;
    return deadline;
}

uint32_t vps_deadline_remaining(const vps_deadline *deadline)
{
    ULONGLONG now;
    ULONGLONG remaining;

    if (deadline == NULL) {
        return 0U;
    }
    now = GetTickCount64();
    if (now >= deadline->expires_at_ms) {
        return 0U;
    }
    remaining = deadline->expires_at_ms - now;
    return remaining > (ULONGLONG)UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

vps_wait_result vps_wait_socket(PGconn *connection,
                                vps_wait_interest interest,
                                const vps_deadline *deadline,
                                HANDLE interrupt_event)
{
    int socket_value;

    if (connection == NULL || deadline == NULL) {
        return VPS_WAIT_FAILED;
    }
    socket_value = PQsocket(connection);
    if (socket_value < 0) {
        return VPS_WAIT_FAILED;
    }

    return vps_wait_raw_socket(socket_value, interest, deadline, interrupt_event);
}

vps_wait_result vps_wait_raw_socket(int socket_value,
                                    vps_wait_interest interest,
                                    const vps_deadline *deadline,
                                    HANDLE interrupt_event)
{
    SOCKET descriptor;

    if (socket_value < 0 || deadline == NULL) {
        return VPS_WAIT_FAILED;
    }

    descriptor = (SOCKET)(uintptr_t)(unsigned int)socket_value;
    for (;;) {
        uint32_t remaining = vps_deadline_remaining(deadline);
        uint32_t slice;
        struct timeval timeout;
        fd_set read_set;
        fd_set write_set;
        fd_set error_set;
        int select_result;

        if (remaining == 0U) {
            return VPS_WAIT_TIMEOUT;
        }
        if (interrupt_event != NULL && WaitForSingleObject(interrupt_event, 0U) == WAIT_OBJECT_0) {
            return VPS_WAIT_INTERRUPTED;
        }
        slice = remaining > 50U && interrupt_event != NULL ? 50U : remaining;
        timeout.tv_sec = (long)(slice / 1000U);
        timeout.tv_usec = (long)((slice % 1000U) * 1000U);
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_ZERO(&error_set);
        if (interest == VPS_WAIT_READ) {
            FD_SET(descriptor, &read_set);
        } else {
            FD_SET(descriptor, &write_set);
        }
        FD_SET(descriptor, &error_set);
        select_result = select(0, &read_set, &write_set, &error_set, &timeout);
        if (select_result > 0) {
            return FD_ISSET(descriptor, &error_set) ? VPS_WAIT_FAILED : VPS_WAIT_READY;
        }
        if (select_result < 0) {
            return VPS_WAIT_FAILED;
        }
    }
}

vps_connect_result vps_connect_async(const vps_connect_config *config,
                                     uint32_t timeout_ms,
                                     HANDLE interrupt_event,
                                     vps_connect_outcome *outcome)
{
    static const char *const keywords[] = {
        "host", "port", "user", "password", "dbname", "sslmode", "client_encoding", NULL
    };
    const char *values[8];
    vps_deadline deadline;
    uint64_t started_at;
    PGconn *connection;
    PostgresPollingStatusType poll_status;

    if (outcome == NULL) {
        return VPS_CONNECT_INVALID_INPUT;
    }
    memset(outcome, 0, sizeof(*outcome));
    outcome->result = VPS_CONNECT_INVALID_INPUT;
    outcome->last_poll = PGRES_POLLING_FAILED;
    if (config == NULL || config->host == NULL || config->port == NULL || config->user == NULL ||
        config->database == NULL || config->sslmode == NULL || timeout_ms == 0U) {
        return outcome->result;
    }

    values[0] = config->host;
    values[1] = config->port;
    values[2] = config->user;
    values[3] = config->password;
    values[4] = config->database;
    values[5] = config->sslmode;
    values[6] = "UTF8";
    values[7] = NULL;
    started_at = vps_monotonic_ms();
    deadline = vps_deadline_after(timeout_ms);
    connection = PQconnectStartParams(keywords, values, 0);
    outcome->connection = connection;
    if (connection == NULL) {
        outcome->result = VPS_CONNECT_FAILED;
        outcome->duration_ms = vps_monotonic_ms() - started_at;
        return outcome->result;
    }
    if (PQsetnonblocking(connection, 1) != 0) {
        outcome->result = VPS_CONNECT_FAILED;
        outcome->duration_ms = vps_monotonic_ms() - started_at;
        return outcome->result;
    }

    for (;;) {
        vps_wait_interest interest;
        vps_wait_result wait_result;
        char fields[128];

        poll_status = PQconnectPoll(connection);
        outcome->last_poll = poll_status;
        outcome->poll_count++;
        (void)snprintf(fields, sizeof(fields), "state=%s remaining_ms=%u poll_count=%u",
                       vps_poll_name(poll_status), vps_deadline_remaining(&deadline), outcome->poll_count);
        vps_log("debug", "connect_poll", fields);
        if (poll_status == PGRES_POLLING_OK) {
            outcome->result = PQstatus(connection) == CONNECTION_OK ? VPS_CONNECT_OK : VPS_CONNECT_FAILED;
            break;
        }
        if (poll_status == PGRES_POLLING_FAILED) {
            outcome->result = VPS_CONNECT_FAILED;
            break;
        }
        if (poll_status == PGRES_POLLING_ACTIVE) {
            if (vps_deadline_remaining(&deadline) == 0U) {
                outcome->result = VPS_CONNECT_TIMEOUT;
                break;
            }
            continue;
        }
        interest = poll_status == PGRES_POLLING_READING ? VPS_WAIT_READ : VPS_WAIT_WRITE;
        outcome->wait_count++;
        (void)snprintf(fields, sizeof(fields), "interest=%s remaining_ms=%u wait_count=%u",
                       interest == VPS_WAIT_READ ? "read" : "write", vps_deadline_remaining(&deadline), outcome->wait_count);
        vps_log("debug", "connect_wait", fields);
        wait_result = vps_wait_socket(connection, interest, &deadline, interrupt_event);
        if (wait_result == VPS_WAIT_TIMEOUT) {
            poll_status = PQconnectPoll(connection);
            outcome->last_poll = poll_status;
            outcome->poll_count++;
            outcome->result = poll_status == PGRES_POLLING_FAILED ? VPS_CONNECT_FAILED : VPS_CONNECT_TIMEOUT;
            break;
        }
        if (wait_result == VPS_WAIT_INTERRUPTED) {
            outcome->result = VPS_CONNECT_INTERRUPTED;
            break;
        }
        if (wait_result == VPS_WAIT_FAILED && vps_deadline_remaining(&deadline) == 0U) {
            outcome->result = VPS_CONNECT_TIMEOUT;
            break;
        }
        if (wait_result == VPS_WAIT_FAILED) {
            outcome->result = VPS_CONNECT_FAILED;
            break;
        }
    }
    outcome->duration_ms = vps_monotonic_ms() - started_at;
    return outcome->result;
}

const char *vps_poll_name(PostgresPollingStatusType status)
{
    switch (status) {
        case PGRES_POLLING_FAILED: return "failed";
        case PGRES_POLLING_READING: return "reading";
        case PGRES_POLLING_WRITING: return "writing";
        case PGRES_POLLING_OK: return "ok";
        case PGRES_POLLING_ACTIVE: return "active";
        default: return "unknown";
    }
}

const char *vps_connect_result_name(vps_connect_result result)
{
    switch (result) {
        case VPS_CONNECT_OK: return "ok";
        case VPS_CONNECT_FAILED: return "failed";
        case VPS_CONNECT_TIMEOUT: return "timeout";
        case VPS_CONNECT_INTERRUPTED: return "interrupted";
        case VPS_CONNECT_INVALID_INPUT: return "invalid_input";
        default: return "unknown";
    }
}

const char *vps_result_status_name(ExecStatusType status)
{
    switch (status) {
        case PGRES_EMPTY_QUERY: return "empty_query";
        case PGRES_COMMAND_OK: return "command_ok";
        case PGRES_TUPLES_OK: return "tuples_ok";
        case PGRES_COPY_OUT: return "copy_out";
        case PGRES_COPY_IN: return "copy_in";
        case PGRES_BAD_RESPONSE: return "bad_response";
        case PGRES_NONFATAL_ERROR: return "nonfatal_error";
        case PGRES_FATAL_ERROR: return "fatal_error";
        case PGRES_COPY_BOTH: return "copy_both";
        case PGRES_SINGLE_TUPLE: return "single_tuple";
        case PGRES_PIPELINE_SYNC: return "pipeline_sync";
        case PGRES_PIPELINE_ABORTED: return "pipeline_aborted";
        case PGRES_TUPLES_CHUNK: return "tuples_chunk";
        default: return "unknown";
    }
}

void vps_log(const char *level, const char *event, const char *fields)
{
    printf("[vps] level=%s event=%s%s%s\n",
           level == NULL ? "info" : level,
           event == NULL ? "unknown" : event,
           fields == NULL || fields[0] == '\0' ? "" : " ",
           fields == NULL ? "" : fields);
}

int vps_flush_async(PGconn *connection, const vps_deadline *deadline, HANDLE interrupt_event)
{
    for (;;) {
        int flush_result = PQflush(connection);
        if (flush_result == 0) {
            return 1;
        }
        if (flush_result < 0) {
            return 0;
        }
        if (vps_wait_socket(connection, VPS_WAIT_WRITE, deadline, interrupt_event) != VPS_WAIT_READY) {
            return 0;
        }
    }
}

PGresult *vps_get_result_async(PGconn *connection,
                               const vps_deadline *deadline,
                               HANDLE interrupt_event,
                               int *operation_ok)
{
    if (operation_ok == NULL) {
        return NULL;
    }
    *operation_ok = 0;
    for (;;) {
        if (PQconsumeInput(connection) == 0) {
            return NULL;
        }
        if (PQisBusy(connection) == 0) {
            *operation_ok = 1;
            return PQgetResult(connection);
        }
        if (vps_wait_socket(connection, VPS_WAIT_READ, deadline, interrupt_event) != VPS_WAIT_READY) {
            return NULL;
        }
    }
}

int vps_load_test_config(vps_connect_config *config)
{
    if (config == NULL) {
        return 0;
    }
    config->host = getenv("VPS_TEST_HOST");
    config->port = getenv("VPS_TEST_PORT");
    config->user = getenv("VPS_TEST_USER");
    config->password = getenv("VPS_TEST_PASSWORD");
    config->database = getenv("VPS_TEST_DATABASE");
    config->sslmode = getenv("VPS_TEST_SSLMODE");
    return config->host != NULL && config->host[0] != '\0' &&
           config->port != NULL && config->port[0] != '\0' &&
           config->user != NULL && config->user[0] != '\0' &&
           config->database != NULL && config->database[0] != '\0' &&
           config->sslmode != NULL && strcmp(config->sslmode, "disable") == 0;
}

PGconn *vps_connect_test_server(uint32_t timeout_ms)
{
    vps_connect_config config;
    vps_connect_outcome outcome;

    if (!vps_load_test_config(&config)) {
        vps_log("error", "missing_test_configuration", NULL);
        return NULL;
    }
    if (vps_connect_async(&config, timeout_ms, NULL, &outcome) != VPS_CONNECT_OK) {
        if (outcome.connection != NULL) {
            PQfinish(outcome.connection);
        }
        vps_log("error", "server_connect_failed", NULL);
        return NULL;
    }
    return outcome.connection;
}

const char *vps_sqlstate(PGresult *result)
{
    const char *state;

    if (result == NULL) {
        return "none";
    }
    state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
    return state == NULL || state[0] == '\0' ? "none" : state;
}
