#include "vps_libpq_client_session.h"

#include <libpq-fe.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VPS_SESSION_ENV_HOST "VPS_SESSION_TEST_HOST"
#define VPS_SESSION_ENV_PORT "VPS_SESSION_TEST_PORT"
#define VPS_SESSION_ENV_USER "VPS_SESSION_TEST_USER"
#define VPS_SESSION_ENV_PASSWORD "VPS_SESSION_TEST_PASSWORD"
#define VPS_SESSION_ENV_DBNAME "VPS_SESSION_TEST_DBNAME"

typedef struct VpsSessionTestEnvironment {
    const char *host;
    const char *port;
    const char *user;
    const char *password;
    const char *dbname;
    const char *sslmode;
    const char *sslrootcert;
} VpsSessionTestEnvironment;

static int vps_session_test_environment(
    VpsSessionTestEnvironment *environment)
{
    environment->host = getenv(VPS_SESSION_ENV_HOST);
    environment->port = getenv(VPS_SESSION_ENV_PORT);
    environment->user = getenv(VPS_SESSION_ENV_USER);
    environment->password = getenv(VPS_SESSION_ENV_PASSWORD);
    environment->dbname = getenv(VPS_SESSION_ENV_DBNAME);
    environment->sslmode = getenv("VPS_SESSION_TEST_SSLMODE");
    environment->sslrootcert = getenv("VPS_SESSION_TEST_SSLROOTCERT");
    if (environment->sslmode == NULL) environment->sslmode = "disable";
    return environment->host != NULL && environment->port != NULL &&
           environment->user != NULL && environment->password != NULL &&
           environment->dbname != NULL;
}

static PGconn *vps_session_test_connect(
    const VpsSessionTestEnvironment *environment)
{
    const char *keywords[] = {
        "host", "port", "user", "password", "dbname", "sslmode",
        "sslrootcert", "connect_timeout", NULL};
    const char *values[] = {
        environment->host, environment->port, environment->user,
        environment->password, environment->dbname, environment->sslmode,
        environment->sslrootcert, "10", NULL};
    return PQconnectdbParams(keywords, values, 0);
}

static const char *vps_session_test_failure_class(PGconn *connection)
{
    const char *message = connection == NULL ? NULL : PQerrorMessage(connection);
    if (message == NULL) return "connection";
    if (strstr(message, "password authentication failed") != NULL) {
        return "authentication";
    }
    if (strstr(message, "no pg_hba.conf entry") != NULL) return "access_policy";
    if (strstr(message, "timeout") != NULL) return "timeout";
    if (strstr(message, "Connection refused") != NULL ||
        strstr(message, "actively refused") != NULL) return "refused";
    if (strstr(message, "could not translate host name") != NULL) {
        return "name_resolution";
    }
    return "connection";
}

static int vps_session_test_arguments(VpsParsedArguments *arguments,
                                      const char *mode)
{
    VpsArgumentInput inputs[] = {
        {"service=session_probe", 21U},
        {"source=table", 12U},
        {"schema=public", 13U},
        {"table=session_probe", 19U},
        {NULL, 0U}};
    inputs[4].text = mode;
    inputs[4].length = strlen(mode);
    return vps_arguments_parse(arguments, inputs,
                               sizeof(inputs) / sizeof(inputs[0]), NULL) ==
           VPS_ARGUMENTS_OK;
}

int main(void)
{
    VpsSessionTestEnvironment environment;
    VpsAllocator allocator;
    VpsParsedArguments arguments;
    VpsConnectionConfig connection_config;
    VpsSessionPlan baseline;
    VpsSessionPlan drift;
    VpsSessionBuildOptions baseline_options = {NULL, 0U, 2000U};
    VpsSessionBuildOptions drift_options = {"Europe/Moscow", 13U, 0U};
    PGconn *connection = NULL;
    VpsSessionResult session_result;
    int arguments_initialized = 0;
    int config_initialized = 0;
    int passed = 0;
    if (!vps_session_test_environment(&environment)) {
        (void)printf(
            "session_probe=all status=skipped reason=runtime_env_missing\n");
        return 77;
    }
    connection = vps_session_test_connect(&environment);
    if (connection == NULL || PQstatus(connection) != CONNECTION_OK) {
        (void)printf("session_probe=connect status=failed failure_class=%s\n",
                     vps_session_test_failure_class(connection));
        goto cleanup;
    }
    (void)printf("session_probe=connect status=passed\n");
    if (vps_allocator_system(&allocator) != VPS_MEMORY_OK ||
        vps_arguments_init(&arguments, &allocator,
                           vps_platform_current_operations(), NULL) !=
            VPS_ARGUMENTS_OK) goto cleanup;
    arguments_initialized = 1;
    if (vps_connection_config_init(&connection_config, &allocator,
                                   vps_platform_current_operations(), NULL) !=
        VPS_CONNECTION_STRING_OK) goto cleanup;
    config_initialized = 1;
    connection_config.config.statement_timeout = "1500";
    connection_config.config.lock_timeout = "250";
    if (!vps_session_test_arguments(&arguments, "mode=ro") ||
        vps_session_plan_init(&baseline, NULL) != VPS_SESSION_OK ||
        vps_session_plan_build(&baseline, &connection_config, &arguments,
                               &baseline_options) != VPS_SESSION_OK) {
        goto cleanup;
    }
    session_result = vps_libpq_client_session_apply(
        connection, &baseline, VPS_SESSION_PHASE_CONNECT);
    (void)printf("session_probe=baseline status=%s result=%s\n",
                 session_result == VPS_SESSION_OK ? "passed" : "failed",
                 vps_session_result_name(session_result));
    if (session_result != VPS_SESSION_OK) goto cleanup;
    connection_config.config.application_name = "VirtualPostgreSQL/drift";
    connection_config.config.search_path = "pg_catalog, public";
    connection_config.config.statement_timeout = "0";
    connection_config.config.lock_timeout = "0";
    if (!vps_session_test_arguments(&arguments, "mode=rw") ||
        vps_session_plan_init(&drift, NULL) != VPS_SESSION_OK ||
        vps_session_plan_build(&drift, &connection_config, &arguments,
                               &drift_options) != VPS_SESSION_OK) goto cleanup;
    session_result = vps_libpq_client_session_apply(
        connection, &drift, VPS_SESSION_PHASE_RESET);
    (void)printf("session_probe=drift status=%s result=%s\n",
                 session_result == VPS_SESSION_OK ? "passed" : "failed",
                 vps_session_result_name(session_result));
    if (session_result != VPS_SESSION_OK) goto cleanup;
    session_result = vps_libpq_client_session_apply(
        connection, &baseline, VPS_SESSION_PHASE_RESET);
    (void)printf("session_probe=reset status=%s result=%s\n",
                 session_result == VPS_SESSION_OK ? "passed" : "failed",
                 vps_session_result_name(session_result));
    if (session_result != VPS_SESSION_OK) goto cleanup;
    passed = 1;

cleanup:
    if (connection != NULL) PQfinish(connection);
    if (config_initialized) {
        (void)vps_connection_config_cleanup(&connection_config);
    }
    if (arguments_initialized) (void)vps_arguments_reset(&arguments);
    (void)printf("session_probe=all status=%s phases=connect,drift,reset\n",
                 passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
