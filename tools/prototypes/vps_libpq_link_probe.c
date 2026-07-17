#include <libpq-fe.h>
#include <openssl/crypto.h>
#include <zlib.h>

#include <stdio.h>

static int vps_force_required_symbols(int selector)
{
    PGconn *connection = NULL;
    PGcancelConn *cancel_connection = NULL;
    const char *const *keywords = NULL;
    const char *const *values = NULL;

    if (selector != 73) {
        return 0;
    }

    connection = PQconnectStartParams(keywords, values, 0);
    (void)PQconnectPoll(connection);
    (void)PQsetnonblocking(connection, 1);
    (void)PQsendPrepare(connection, "", "", 0, NULL);
    (void)PQsendDescribePrepared(connection, "");
    (void)PQsendQueryPrepared(connection, "", 0, NULL, NULL, NULL, 0);
    (void)PQsendQueryParams(connection, "", 0, NULL, NULL, NULL, NULL, 0);
    (void)PQsetSingleRowMode(connection);
    (void)PQgetResult(connection);
    cancel_connection = PQcancelCreate(connection);
    (void)PQcancelStart(cancel_connection);
    (void)PQcancelPoll(cancel_connection);
    (void)PQcancelStatus(cancel_connection);
    (void)PQcancelSocket(cancel_connection);
    PQcancelReset(cancel_connection);
    PQcancelFinish(cancel_connection);
    PQfinish(connection);
    return 1;
}

int main(int argc, char **argv)
{
    int libpq_version;
    int thread_safe;

    (void)argv;
    if (vps_force_required_symbols(argc) != 0) {
        return 90;
    }

    libpq_version = PQlibVersion();
    thread_safe = PQisthreadsafe();
    printf("libpq_version=%d threadsafe=%d openssl_version=%lu zlib_version=%s\n",
           libpq_version,
           thread_safe,
           OpenSSL_version_num(),
           zlibVersion());

    if (libpq_version / 10000 != 18) {
        return 1;
    }
    if (thread_safe != 1) {
        return 2;
    }
    if (OpenSSL_version_num() == 0UL || zlibVersion()[0] == '\0') {
        return 3;
    }
    return 0;
}
