#include "vps_proto_common.h"

#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VPS_WORKER_COUNT 8
#define VPS_ROWS_PER_SCAN 1000U

static const char VPS_SQL_SCAN[] = "SELECT g::int4 FROM generate_series(1, 1000) AS g";

typedef enum vps_worker_mode {
    VPS_WORKER_NORMAL,
    VPS_WORKER_CLOSE_ONE,
    VPS_WORKER_FAIL_ONE
} vps_worker_mode;

typedef struct vps_worker {
    unsigned int id;
    vps_worker_mode mode;
    HANDLE start_event;
    volatile LONG *ready_count;
    unsigned int rows;
    unsigned int results;
    unsigned int clears;
    unsigned int connect_count;
    unsigned int finish_count;
    int outcome;
} vps_worker;

static DWORD WINAPI vps_worker_main(void *argument)
{
    vps_worker *worker = (vps_worker *)argument;
    PGconn *connection = vps_connect_test_server(5000U);
    vps_deadline deadline = vps_deadline_after(30000U);
    int operation_ok = 0;
    int terminal_ok = 0;

    if (connection == NULL) {
        worker->outcome = 0;
        (void)InterlockedIncrement(worker->ready_count);
        return 0U;
    }
    worker->connect_count = 1U;
    (void)InterlockedIncrement(worker->ready_count);
    if (WaitForSingleObject(worker->start_event, 10000U) != WAIT_OBJECT_0) {
        goto cleanup;
    }
    if (PQsendQueryParams(connection, VPS_SQL_SCAN, 0, NULL, NULL, NULL, NULL, 1) == 0 ||
        PQsetSingleRowMode(connection) == 0 || !vps_flush_async(connection, &deadline, NULL)) {
        goto cleanup;
    }
    if (worker->mode == VPS_WORKER_FAIL_ONE && worker->id == 0U) {
        int socket_value = PQsocket(connection);
        if (socket_value >= 0) {
            (void)shutdown((SOCKET)(uintptr_t)(unsigned int)socket_value, SD_BOTH);
        }
    }

    for (;;) {
        PGresult *result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
        ExecStatusType status;
        if (!operation_ok) {
            worker->outcome = worker->mode == VPS_WORKER_FAIL_ONE && worker->id == 0U;
            break;
        }
        if (result == NULL) {
            worker->outcome = terminal_ok && worker->rows == VPS_ROWS_PER_SCAN;
            break;
        }
        worker->results++;
        status = PQresultStatus(result);
        if (status == PGRES_SINGLE_TUPLE) {
            worker->rows++;
        } else if (status == PGRES_TUPLES_OK) {
            terminal_ok = 1;
        }
        PQclear(result);
        worker->clears++;
        if (worker->mode == VPS_WORKER_CLOSE_ONE && worker->id == 0U && worker->rows == 1U) {
            worker->outcome = 1;
            break;
        }
    }

cleanup:
    PQfinish(connection);
    worker->finish_count = 1U;
    return 0U;
}

static int vps_offline_ownership_cycles(unsigned int cycles)
{
    unsigned int cycle;
    for (cycle = 0U; cycle < cycles; ++cycle) {
        unsigned int index;
        vps_worker *workers = (vps_worker *)calloc(VPS_WORKER_COUNT, sizeof(*workers));
        if (workers == NULL) {
            return 0;
        }
        for (index = 0U; index < VPS_WORKER_COUNT; ++index) {
            workers[index].id = index;
            workers[index].connect_count = 1U;
            workers[index].finish_count = 1U;
        }
        free(workers);
    }
    return 1;
}

static SIZE_T vps_private_bytes(void)
{
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof(counters))) {
        return 0U;
    }
    return counters.PrivateUsage;
}

int main(int argc, char **argv)
{
    vps_worker_mode mode = VPS_WORKER_NORMAL;
    vps_worker workers[VPS_WORKER_COUNT];
    HANDLE threads[VPS_WORKER_COUNT];
    HANDLE start_event;
    volatile LONG ready_count = 0;
    DWORD handles_before = 0U;
    DWORD handles_after = 0U;
    SIZE_T memory_before;
    SIZE_T memory_after;
    unsigned int index;
    unsigned int rows = 0U;
    unsigned int connections = 0U;
    unsigned int finishes = 0U;
    unsigned int results = 0U;
    unsigned int clears = 0U;
    int ok = 1;
    char fields[256];

    if (argc != 2) {
        vps_log("error", "usage", "modes=normal,close-one,one-failure");
        return 64;
    }
    if (strcmp(argv[1], "close-one") == 0) { mode = VPS_WORKER_CLOSE_ONE; }
    else if (strcmp(argv[1], "one-failure") == 0) { mode = VPS_WORKER_FAIL_ONE; }
    else if (strcmp(argv[1], "normal") != 0) { return 64; }

    if (!vps_offline_ownership_cycles(1000U)) {
        return 65;
    }
    (void)GetProcessHandleCount(GetCurrentProcess(), &handles_before);
    memory_before = vps_private_bytes();
    start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (start_event == NULL) {
        return 66;
    }
    memset(workers, 0, sizeof(workers));
    memset(threads, 0, sizeof(threads));
    for (index = 0U; index < VPS_WORKER_COUNT; ++index) {
        workers[index].id = index;
        workers[index].mode = mode;
        workers[index].start_event = start_event;
        workers[index].ready_count = &ready_count;
        threads[index] = CreateThread(NULL, 0U, vps_worker_main, &workers[index], 0U, NULL);
        if (threads[index] == NULL) {
            ok = 0;
            break;
        }
    }
    while (ok && InterlockedCompareExchange(&ready_count, 0, 0) < VPS_WORKER_COUNT) {
        Sleep(10U);
    }
    (void)SetEvent(start_event);
    if (ok && WaitForMultipleObjects(VPS_WORKER_COUNT, threads, TRUE, 60000U) != WAIT_OBJECT_0) {
        ok = 0;
    }
    for (index = 0U; index < VPS_WORKER_COUNT; ++index) {
        if (threads[index] != NULL) {
            CloseHandle(threads[index]);
        }
        connections += workers[index].connect_count;
        finishes += workers[index].finish_count;
        rows += workers[index].rows;
        results += workers[index].results;
        clears += workers[index].clears;
        if (!workers[index].outcome) {
            ok = 0;
        }
    }
    CloseHandle(start_event);
    memory_after = vps_private_bytes();
    (void)GetProcessHandleCount(GetCurrentProcess(), &handles_after);

    if (connections != VPS_WORKER_COUNT || finishes != VPS_WORKER_COUNT || results != clears) {
        ok = 0;
    }
    if (mode == VPS_WORKER_NORMAL && rows != VPS_WORKER_COUNT * VPS_ROWS_PER_SCAN) { ok = 0; }
    if (mode == VPS_WORKER_CLOSE_ONE && rows != (VPS_WORKER_COUNT - 1U) * VPS_ROWS_PER_SCAN + 1U) { ok = 0; }
    if (mode == VPS_WORKER_FAIL_ONE && rows != (VPS_WORKER_COUNT - 1U) * VPS_ROWS_PER_SCAN) { ok = 0; }
    /* Winsock/OpenSSL retain bounded process-global handles after first use. */
    if (handles_after > handles_before + 32U) { ok = 0; }

    (void)snprintf(fields, sizeof(fields),
                   "workers=8 rows=%u results=%u clears=%u connections=%u finishes=%u offline_cycles=1000 handle_delta=%ld rss_delta=%lld outcome=%s",
                   rows, results, clears, connections, finishes,
                   (long)handles_after - (long)handles_before,
                   (long long)memory_after - (long long)memory_before,
                   ok ? "pass" : "fail");
    vps_log(ok ? "info" : "error", "concurrency_complete", fields);
    return ok ? 0 : 1;
}
