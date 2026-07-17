#include "vps_proto_common.h"

#include <psapi.h>
#include <stdio.h>
#include <string.h>

#define VPS_STREAM_ROWS 1000000U

static const char VPS_SQL_MILLION[] = "SELECT g::int4 FROM generate_series(1, 1000000) AS g";
static const char VPS_SQL_BEGIN_STREAM[] = "BEGIN";

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

static int vps_begin(PGconn *connection)
{
    vps_deadline deadline = vps_deadline_after(5000U);
    int operation_ok = 0;
    PGresult *result;
    int ok;
    if (PQsendQueryParams(connection, VPS_SQL_BEGIN_STREAM, 0, NULL, NULL, NULL, NULL, 0) == 0 ||
        !vps_flush_async(connection, &deadline, NULL)) { return 0; }
    result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
    ok = operation_ok && result != NULL && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (result != NULL) { PQclear(result); }
    result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
    if (result != NULL) { PQclear(result); return 0; }
    return operation_ok && ok && PQtransactionStatus(connection) == PQTRANS_INTRANS;
}

int main(int argc, char **argv)
{
    int loss_before;
    int loss_after;
    int transaction_loss;
    PGconn *connection;
    vps_deadline deadline = vps_deadline_after(120000U);
    SIZE_T baseline;
    SIZE_T peak;
    uint64_t started_at;
    uint64_t first_row_ms = 0U;
    unsigned int rows = 0U;
    unsigned int expected = 1U;
    unsigned int results = 0U;
    unsigned int clears = 0U;
    int operation_ok = 0;
    int terminal_ok = 0;
    int saw_loss = 0;
    int ok;
    char fields[320];

    if (argc != 2) {
        vps_log("error", "usage", "modes=million,loss-before-row,loss-after-row,loss-in-transaction");
        return 64;
    }
    loss_before = strcmp(argv[1], "loss-before-row") == 0;
    loss_after = strcmp(argv[1], "loss-after-row") == 0;
    transaction_loss = strcmp(argv[1], "loss-in-transaction") == 0;
    if (!loss_before && !loss_after && !transaction_loss && strcmp(argv[1], "million") != 0) { return 64; }

    connection = vps_connect_test_server(5000U);
    if (connection == NULL) { return 65; }
    if (transaction_loss && !vps_begin(connection)) { PQfinish(connection); return 1; }
    baseline = vps_private_bytes();
    peak = baseline;
    started_at = (uint64_t)GetTickCount64();
    if (PQsendQueryParams(connection, VPS_SQL_MILLION, 0, NULL, NULL, NULL, NULL, 1) == 0 ||
        PQsetSingleRowMode(connection) == 0 || !vps_flush_async(connection, &deadline, NULL)) {
        PQfinish(connection);
        return 1;
    }
    if (loss_before) {
        int socket_value = PQsocket(connection);
        if (socket_value >= 0) { (void)shutdown((SOCKET)(uintptr_t)(unsigned int)socket_value, SD_BOTH); }
    }

    for (;;) {
        PGresult *result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
        ExecStatusType status;
        if (!operation_ok) { saw_loss = 1; break; }
        if (result == NULL) { break; }
        results++;
        status = PQresultStatus(result);
        if (status == PGRES_SINGLE_TUPLE) {
            uint32_t network_value;
            unsigned int value;
            if (PQgetlength(result, 0, 0) != 4) { PQclear(result); clears++; break; }
            memcpy(&network_value, PQgetvalue(result, 0, 0), 4U);
            value = ntohl(network_value);
            if (value != expected) { PQclear(result); clears++; break; }
            expected++;
            rows++;
            if (rows == 1U) { first_row_ms = (uint64_t)GetTickCount64() - started_at; }
            if ((rows % 10000U) == 0U) {
                SIZE_T sample = vps_private_bytes();
                if (sample > peak) { peak = sample; }
            }
            if ((loss_after || transaction_loss) && rows == 10U) {
                int socket_value = PQsocket(connection);
                if (socket_value >= 0) { (void)shutdown((SOCKET)(uintptr_t)(unsigned int)socket_value, SD_BOTH); }
            }
        } else if (status == PGRES_TUPLES_OK) {
            terminal_ok = 1;
        } else if (status == PGRES_FATAL_ERROR) {
            saw_loss = 1;
        }
        PQclear(result);
        clears++;
    }
    if (loss_before) {
        ok = saw_loss && rows == 0U;
    } else if (loss_after || transaction_loss) {
        ok = saw_loss && rows >= 10U && !terminal_ok;
    } else {
        ok = operation_ok && terminal_ok && rows == VPS_STREAM_ROWS && results == clears &&
             peak >= baseline && peak - baseline < (SIZE_T)(64U * 1024U * 1024U);
    }
    (void)snprintf(fields, sizeof(fields),
                   "mode=%s rows=%u results=%u clears=%u first_row_ms=%llu duration_ms=%llu rss_baseline=%llu rss_peak=%llu retry=%s decision=%s outcome=%s",
                   argv[1], rows, results, clears,
                   (unsigned long long)first_row_ms,
                   (unsigned long long)((uint64_t)GetTickCount64() - started_at),
                   (unsigned long long)baseline, (unsigned long long)peak,
                   loss_before ? "eligible_not_attempted" : "forbidden",
                   (loss_before || loss_after || transaction_loss) ? "destroy" : "clean",
                   ok ? "pass" : "fail");
    vps_log(ok ? "info" : "error", "stream_complete", fields);
    PQfinish(connection);
    return ok ? 0 : 1;
}
