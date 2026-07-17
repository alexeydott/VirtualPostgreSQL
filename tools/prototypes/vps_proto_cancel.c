#include "vps_proto_common.h"

#include <stdio.h>
#include <string.h>

static const char VPS_SQL_SLOW_STREAM[] =
    "SELECT g::int4, pg_sleep((g * 0 + 5)::double precision / 1000) "
    "FROM generate_series(1, 1000) AS g";

typedef struct vps_trigger {
    HANDLE arm_event;
    HANDLE fired_event;
    HANDLE thread;
} vps_trigger;

typedef enum vps_cancel_result {
    VPS_CANCEL_OK,
    VPS_CANCEL_FAILED,
    VPS_CANCEL_TIMEOUT
} vps_cancel_result;

static DWORD WINAPI vps_trigger_thread(void *argument)
{
    vps_trigger *trigger = (vps_trigger *)argument;
    if (WaitForSingleObject(trigger->arm_event, INFINITE) == WAIT_OBJECT_0) {
        (void)SetEvent(trigger->fired_event);
    }
    return 0U;
}

static int vps_trigger_start(vps_trigger *trigger)
{
    memset(trigger, 0, sizeof(*trigger));
    trigger->arm_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    trigger->fired_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (trigger->arm_event == NULL || trigger->fired_event == NULL) {
        return 0;
    }
    trigger->thread = CreateThread(NULL, 0U, vps_trigger_thread, trigger, 0U, NULL);
    return trigger->thread != NULL;
}

static void vps_trigger_finish(vps_trigger *trigger)
{
    if (trigger->arm_event != NULL) {
        (void)SetEvent(trigger->arm_event);
    }
    if (trigger->thread != NULL) {
        (void)WaitForSingleObject(trigger->thread, 2000U);
        CloseHandle(trigger->thread);
    }
    if (trigger->fired_event != NULL) { CloseHandle(trigger->fired_event); }
    if (trigger->arm_event != NULL) { CloseHandle(trigger->arm_event); }
    memset(trigger, 0, sizeof(*trigger));
}

static int vps_trigger_fire(vps_trigger *trigger)
{
    return SetEvent(trigger->arm_event) != 0 &&
           WaitForSingleObject(trigger->fired_event, 2000U) == WAIT_OBJECT_0;
}

static vps_cancel_result vps_secure_cancel(PGconn *connection,
                                           uint32_t timeout_ms,
                                           int force_timeout,
                                           int break_network,
                                           unsigned int *create_count,
                                           unsigned int *finish_count)
{
    PGcancelConn *cancel_connection;
    vps_deadline deadline;
    vps_cancel_result result = VPS_CANCEL_FAILED;
    PostgresPollingStatusType status;
    char fields[160];

    cancel_connection = PQcancelCreate(connection);
    if (cancel_connection == NULL) {
        return VPS_CANCEL_FAILED;
    }
    (*create_count)++;
    if (PQcancelStart(cancel_connection) == 0) {
        PQcancelFinish(cancel_connection);
        (*finish_count)++;
        return VPS_CANCEL_FAILED;
    }
    deadline = vps_deadline_after(timeout_ms);
    if (force_timeout) {
        Sleep(timeout_ms + 1U);
    }
    if (break_network) {
        int socket_value = PQcancelSocket(cancel_connection);
        if (socket_value >= 0) {
            (void)shutdown((SOCKET)(uintptr_t)(unsigned int)socket_value, SD_BOTH);
        }
        socket_value = PQsocket(connection);
        if (socket_value >= 0) {
            (void)shutdown((SOCKET)(uintptr_t)(unsigned int)socket_value, SD_BOTH);
        }
    }

    for (;;) {
        vps_wait_result wait_result;
        vps_wait_interest interest;
        uint32_t remaining = vps_deadline_remaining(&deadline);

        if (remaining == 0U) {
            result = VPS_CANCEL_TIMEOUT;
            break;
        }
        status = PQcancelPoll(cancel_connection);
        (void)snprintf(fields, sizeof(fields), "phase=poll status=%s remaining_ms=%u",
                       vps_poll_name(status), remaining);
        vps_log("debug", "cancel_transition", fields);
        if (status == PGRES_POLLING_OK) {
            result = PQcancelStatus(cancel_connection) == CONNECTION_OK ? VPS_CANCEL_OK : VPS_CANCEL_FAILED;
            break;
        }
        if (status == PGRES_POLLING_FAILED) {
            result = VPS_CANCEL_FAILED;
            break;
        }
        interest = status == PGRES_POLLING_READING ? VPS_WAIT_READ : VPS_WAIT_WRITE;
        wait_result = vps_wait_raw_socket(PQcancelSocket(cancel_connection), interest, &deadline, NULL);
        if (wait_result == VPS_WAIT_TIMEOUT) {
            result = VPS_CANCEL_TIMEOUT;
            break;
        }
        if (wait_result != VPS_WAIT_READY) {
            result = VPS_CANCEL_FAILED;
            break;
        }
    }
    if (result == VPS_CANCEL_OK) {
        PQcancelReset(cancel_connection);
    }
    PQcancelFinish(cancel_connection);
    (*finish_count)++;
    return result;
}

static int vps_drain_after_cancel(PGconn *connection,
                                  unsigned int *published_rows,
                                  unsigned int *result_count,
                                  unsigned int *clear_count)
{
    vps_deadline deadline = vps_deadline_after(5000U);
    int operation_ok = 0;
    int saw_cancel = 0;

    for (;;) {
        PGresult *result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
        ExecStatusType status;
        if (!operation_ok) {
            return 0;
        }
        if (result == NULL) {
            break;
        }
        (*result_count)++;
        status = PQresultStatus(result);
        if (status == PGRES_SINGLE_TUPLE) {
            (*published_rows)++;
        } else if (status == PGRES_FATAL_ERROR && strcmp(vps_sqlstate(result), "57014") == 0) {
            saw_cancel = 1;
        }
        PQclear(result);
        (*clear_count)++;
    }
    return saw_cancel;
}

static int vps_run_cancel_case(const char *mode)
{
    PGconn *connection = NULL;
    vps_trigger trigger;
    vps_deadline query_deadline = vps_deadline_after(15000U);
    vps_cancel_result cancel_result;
    unsigned int cancel_create_count = 0U;
    unsigned int cancel_finish_count = 0U;
    unsigned int result_count = 0U;
    unsigned int clear_count = 0U;
    unsigned int rows = 0U;
    unsigned int trigger_rows = 0U;
    int force_timeout = strcmp(mode, "cancel-timeout") == 0;
    int break_network = strcmp(mode, "broken-network") == 0;
    int before_send = strcmp(mode, "before-send") == 0;
    int drain_ok = 1;
    int reusable;
    int ok = 0;
    char fields[256];

    if (strcmp(mode, "wait") == 0) { trigger_rows = 0U; }
    else if (strcmp(mode, "first-row") == 0) { trigger_rows = 1U; }
    else if (strcmp(mode, "between-rows") == 0) { trigger_rows = 3U; }
    else if (!before_send && !force_timeout && !break_network) { return 0; }

    connection = vps_connect_test_server(5000U);
    if (connection == NULL || !vps_trigger_start(&trigger)) {
        if (connection != NULL) { PQfinish(connection); }
        return 0;
    }

    if (before_send) {
        if (!vps_trigger_fire(&trigger)) { goto cleanup; }
        cancel_result = vps_secure_cancel(connection, 2000U, 0, 0, &cancel_create_count, &cancel_finish_count);
    } else {
        int operation_ok = 0;
        if (PQsendQueryParams(connection, VPS_SQL_SLOW_STREAM, 0, NULL, NULL, NULL, NULL, 1) == 0 ||
            PQsetSingleRowMode(connection) == 0 || !vps_flush_async(connection, &query_deadline, NULL)) {
            goto cleanup;
        }
        if (trigger_rows == 0U || force_timeout || break_network) {
            if (!vps_trigger_fire(&trigger)) { goto cleanup; }
        } else {
            while (rows < trigger_rows) {
                PGresult *result = vps_get_result_async(connection, &query_deadline, NULL, &operation_ok);
                if (!operation_ok || result == NULL) {
                    if (result != NULL) { PQclear(result); }
                    goto cleanup;
                }
                result_count++;
                if (PQresultStatus(result) != PGRES_SINGLE_TUPLE) {
                    PQclear(result);
                    clear_count++;
                    goto cleanup;
                }
                rows++;
                PQclear(result);
                clear_count++;
            }
            if (!vps_trigger_fire(&trigger)) { goto cleanup; }
        }
        cancel_result = vps_secure_cancel(connection, force_timeout ? 1U : 2000U,
                                          force_timeout, break_network,
                                          &cancel_create_count, &cancel_finish_count);
        if (cancel_result == VPS_CANCEL_OK) {
            drain_ok = vps_drain_after_cancel(connection, &rows, &result_count, &clear_count);
        } else {
            drain_ok = 0;
        }
    }

    reusable = cancel_result == VPS_CANCEL_OK && drain_ok && PQstatus(connection) == CONNECTION_OK &&
               PQtransactionStatus(connection) == PQTRANS_IDLE && PQisBusy(connection) == 0;
    if (before_send) {
        reusable = cancel_result == VPS_CANCEL_OK && PQstatus(connection) == CONNECTION_OK &&
                   PQtransactionStatus(connection) == PQTRANS_IDLE;
    }
    ok = cancel_create_count == 1U && cancel_finish_count == 1U && result_count == clear_count &&
         ((before_send && reusable) ||
          (!force_timeout && !break_network && reusable) ||
          ((force_timeout || break_network) && !reusable));
    (void)snprintf(fields, sizeof(fields),
                   "trigger=thread_event cancel=%s published_rows=%u results=%u clears=%u decision=%s sqlstate=%s",
                   cancel_result == VPS_CANCEL_OK ? "ok" : (cancel_result == VPS_CANCEL_TIMEOUT ? "timeout" : "failed"),
                   rows, result_count, clear_count, reusable ? "drain_reusable" : "destroy", drain_ok && !before_send ? "57014" : "none");
    vps_log(ok ? "info" : "error", "cancel_complete", fields);

cleanup:
    vps_trigger_finish(&trigger);
    if (connection != NULL) {
        PQfinish(connection);
    }
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        vps_log("error", "usage", "modes=before-send,wait,first-row,between-rows,cancel-timeout,broken-network");
        return 64;
    }
    return vps_run_cancel_case(argv[1]) ? 0 : 1;
}
