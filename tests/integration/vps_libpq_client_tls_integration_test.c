#include "vps_libpq_client_tls.h"

#include <libpq-fe.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VPS_TLS_ENV_HOST "VPS_TLS_TEST_HOST"
#define VPS_TLS_ENV_PORT "VPS_TLS_TEST_PORT"
#define VPS_TLS_ENV_USER "VPS_TLS_TEST_USER"
#define VPS_TLS_ENV_PASSWORD "VPS_TLS_TEST_PASSWORD"
#define VPS_TLS_ENV_DBNAME "VPS_TLS_TEST_DBNAME"
#define VPS_TLS_ENV_BAD_CA "VPS_TLS_TEST_BAD_CA"

typedef struct VpsTlsTestEnvironment {
    const char *host;
    const char *port;
    const char *user;
    const char *password;
    const char *dbname;
    const char *bad_ca;
} VpsTlsTestEnvironment;

static int vps_tls_test_environment(VpsTlsTestEnvironment *environment)
{
    environment->host = getenv(VPS_TLS_ENV_HOST);
    environment->port = getenv(VPS_TLS_ENV_PORT);
    environment->user = getenv(VPS_TLS_ENV_USER);
    environment->password = getenv(VPS_TLS_ENV_PASSWORD);
    environment->dbname = getenv(VPS_TLS_ENV_DBNAME);
    environment->bad_ca = getenv(VPS_TLS_ENV_BAD_CA);
    return environment->host != NULL && environment->port != NULL &&
           environment->user != NULL && environment->password != NULL &&
           environment->dbname != NULL && environment->bad_ca != NULL;
}

static PGconn *vps_tls_test_connect(const VpsTlsTestEnvironment *environment,
                                    const char *host,
                                    const char *hostaddr,
                                    const char *sslmode,
                                    const char *sslrootcert,
                                    const char *channel_binding)
{
    const char *keywords[] = {
        "host", "hostaddr", "port", "user", "password", "dbname",
        "sslmode", "sslrootcert", "channel_binding", "connect_timeout", NULL};
    const char *values[] = {
        host, hostaddr, environment->port, environment->user,
        environment->password, environment->dbname, sslmode, sslrootcert,
        channel_binding, "10", NULL};
    return PQconnectdbParams(keywords, values, 0);
}

static int vps_tls_test_policy(const char *sslmode,
                               const char *channel_binding,
                               VpsTlsPolicy *policy)
{
    VpsCredentialConfig config;
    VpsTlsPolicyOptions options;
    (void)memset(&config, 0, sizeof(config));
    config.sslmode = sslmode;
    config.channel_binding = channel_binding;
    options.allow_explicit_disable = strcmp(sslmode, "disable") == 0;
    return vps_tls_policy_from_config(&config, &options, policy) == VPS_TLS_OK;
}

static const char *vps_tls_test_failure_class(PGconn *connection)
{
    const char *message = connection == NULL ? NULL : PQerrorMessage(connection);
    if (message == NULL) return "connection";
    if (strstr(message, "channel binding") != NULL) return "channel_binding";
    if (strstr(message, "certificate") != NULL) return "certificate";
    if (strstr(message, "password authentication failed") != NULL) {
        return "authentication";
    }
    if (strstr(message, "no pg_hba.conf entry") != NULL) return "access_policy";
    if (strstr(message, "timeout") != NULL) return "timeout";
    if (strstr(message, "could not translate host name") != NULL) {
        return "name_resolution";
    }
    if (strstr(message, "Connection refused") != NULL) return "refused";
    return "connection";
}

static int vps_tls_test_positive(const VpsTlsTestEnvironment *environment,
                                 const char *name,
                                 const char *sslmode,
                                 const char *channel_binding,
                                 const char *sslrootcert,
                                 char *hostaddr,
                                 size_t hostaddr_size)
{
    VpsTlsPolicy policy;
    VpsTlsOutcome outcome;
    PGconn *connection;
    VpsTlsResult verification = VPS_TLS_CONNECTION_NOT_READY;
    const char *failure_class = "none";
    int passed = 0;
    if (!vps_tls_test_policy(sslmode, channel_binding, &policy)) return 0;
    connection = vps_tls_test_connect(environment, environment->host, NULL,
                                      sslmode, sslrootcert, channel_binding);
    if (connection != NULL) {
        if (PQstatus(connection) == CONNECTION_OK) {
            verification = vps_libpq_client_tls_verify(
                connection, &policy, &outcome, NULL);
            if (verification == VPS_TLS_OK && outcome.ssl_in_use) {
                passed = 1;
                if (hostaddr != NULL) {
                    const char *address = PQhostaddr(connection);
                    size_t length = address == NULL ? 0U : strlen(address);
                    if (length == 0U || length >= hostaddr_size) {
                        passed = 0;
                    } else {
                        (void)memcpy(hostaddr, address, length + 1U);
                    }
                }
            }
        } else {
            failure_class = vps_tls_test_failure_class(connection);
        }
    }
    if (connection != NULL) PQfinish(connection);
    (void)printf("tls_probe=%s status=%s failure_class=%s verification=%s\n",
                 name, passed ? "passed" : "failed", failure_class,
                 vps_tls_result_name(verification));
    return passed;
}

static int vps_tls_test_expected_failure(
    const VpsTlsTestEnvironment *environment,
    const char *name,
    const char *host,
    const char *hostaddr,
    const char *sslrootcert)
{
    PGconn *connection = vps_tls_test_connect(
        environment, host, hostaddr, "verify-full", sslrootcert, "prefer");
    int passed = connection != NULL && PQstatus(connection) != CONNECTION_OK;
    if (connection != NULL) PQfinish(connection);
    (void)printf("tls_probe=%s status=%s\n", name,
                 passed ? "passed" : "failed");
    return passed;
}

static int vps_tls_test_disable(const VpsTlsTestEnvironment *environment)
{
    VpsTlsPolicy policy;
    VpsTlsOutcome outcome;
    PGconn *connection;
    int passed;
    if (!vps_tls_test_policy("disable", "disable", &policy)) return 0;
    connection = vps_tls_test_connect(environment, environment->host, NULL,
                                      "disable", NULL, "disable");
    if (connection == NULL) return 0;
    if (PQstatus(connection) == CONNECTION_OK) {
        passed = vps_libpq_client_tls_verify(connection, &policy, &outcome,
                                              NULL) == VPS_TLS_OK &&
                 !outcome.ssl_in_use;
    } else {
        /* A TLS-only server may reject the explicit plaintext attempt. */
        passed = 1;
    }
    PQfinish(connection);
    (void)printf("tls_probe=disable status=%s\n",
                 passed ? "passed" : "failed");
    return passed;
}

static int vps_tls_test_auth_failure(
    const VpsTlsTestEnvironment *environment)
{
    static const char invalid_password[] = "vps-intentionally-invalid-password";
    VpsTlsTestEnvironment invalid = *environment;
    PGconn *connection;
    int passed;
    invalid.password = invalid_password;
    connection = vps_tls_test_connect(&invalid, invalid.host, NULL,
                                      "verify-full", "system", "prefer");
    passed = connection != NULL && PQstatus(connection) != CONNECTION_OK &&
             strcmp(vps_tls_test_failure_class(connection),
                    "authentication") == 0;
    if (connection != NULL) PQfinish(connection);
    (void)printf("tls_probe=bad_auth status=%s failure_class=authentication\n",
                 passed ? "passed" : "failed");
    return passed;
}

static int vps_tls_test_channel_binding(
    const VpsTlsTestEnvironment *environment)
{
    VpsTlsPolicy policy;
    VpsTlsOutcome outcome;
    PGconn *connection;
    const char *result = "failed";
    int passed = 0;
    if (!vps_tls_test_policy("verify-full", "require", &policy)) return 0;
    connection = vps_tls_test_connect(
        environment, environment->host, NULL, "verify-full", "system",
        "require");
    if (connection != NULL && PQstatus(connection) == CONNECTION_OK) {
        if (vps_libpq_client_tls_verify(connection, &policy, &outcome, NULL) ==
                VPS_TLS_OK &&
            outcome.ssl_in_use &&
            outcome.channel_binding_status ==
                VPS_CHANNEL_BINDING_STATUS_SATISFIED) {
            passed = 1;
            result = "satisfied";
        }
    } else if (connection != NULL &&
               strcmp(vps_tls_test_failure_class(connection),
                      "channel_binding") == 0) {
        /* Requiring unavailable channel binding must fail before connect. */
        passed = 1;
        result = "fail_closed_unsupported";
    }
    if (connection != NULL) PQfinish(connection);
    (void)printf("tls_probe=channel_binding status=%s outcome=%s\n",
                 passed ? "passed" : "failed", result);
    return passed;
}

int main(void)
{
    static const char invalid_hostname[] = "invalid.virtualpostgresql.test";
    VpsTlsTestEnvironment environment;
    char hostaddr[128];
    int passed = 1;
    if (!vps_tls_test_environment(&environment)) {
        (void)printf("tls_probe=all status=skipped reason=runtime_env_missing\n");
        return 77;
    }
    passed &= vps_tls_test_positive(&environment, "require", "require",
                                    "prefer", NULL, NULL, 0U);
    passed &= vps_tls_test_positive(&environment, "prefer", "prefer",
                                    "prefer", NULL, NULL, 0U);
    passed &= vps_tls_test_positive(&environment, "verify_full", "verify-full",
                                    "prefer", "system", hostaddr,
                                    sizeof(hostaddr));
    passed &= vps_tls_test_expected_failure(&environment, "bad_ca",
                                            environment.host, NULL,
                                            environment.bad_ca);
    passed &= vps_tls_test_expected_failure(&environment, "bad_hostname",
                                            invalid_hostname, hostaddr,
                                            "system");
    passed &= vps_tls_test_channel_binding(&environment);
    passed &= vps_tls_test_auth_failure(&environment);
    passed &= vps_tls_test_disable(&environment);
    (void)printf("tls_probe=all status=%s probes=8\n",
                 passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
