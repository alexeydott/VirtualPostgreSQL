#include "vps_libpq_client_tls.h"

#include <libpq-fe.h>

#include <string.h>

static int vps_libpq_attribute_reported(PGconn *connection,
                                        const char *name)
{
    const char *value = PQsslAttribute(connection, name);
    return value != NULL && value[0] != '\0';
}

static void vps_libpq_erase(char *value)
{
    volatile unsigned char *bytes = (volatile unsigned char *)value;
    size_t index;
    size_t length;
    if (value == NULL) return;
    length = strlen(value);
    for (index = 0U; index < length; ++index) bytes[index] = 0U;
}

static int vps_libpq_policy_applied(PGconn *connection,
                                    const VpsTlsPolicy *policy)
{
    PQconninfoOption *options = PQconninfo(connection);
    PQconninfoOption *option;
    const char *sslmode = NULL;
    const char *channel_binding = NULL;
    int matched;
    if (options == NULL) return 0;
    for (option = options; option->keyword != NULL; ++option) {
        if (strcmp(option->keyword, "sslmode") == 0) sslmode = option->val;
        if (strcmp(option->keyword, "channel_binding") == 0) {
            channel_binding = option->val;
        }
    }
    matched = sslmode != NULL && channel_binding != NULL &&
              strcmp(sslmode, vps_tls_mode_name(policy->mode)) == 0 &&
              strcmp(channel_binding,
                     vps_channel_binding_mode_name(policy->channel_binding)) == 0;
    for (option = options; option->keyword != NULL; ++option) {
        vps_libpq_erase(option->val);
    }
    PQconninfoFree(options);
    return matched;
}

VpsTlsResult vps_libpq_client_tls_verify(void *connection,
                                         const VpsTlsPolicy *policy,
                                         VpsTlsOutcome *outcome,
                                         VpsLogger *logger)
{
    PGconn *postgresql_connection = (PGconn *)connection;
    VpsTlsObservation observation = {0};
    if (postgresql_connection == NULL || policy == NULL || outcome == NULL) {
        return VPS_TLS_INVALID_ARGUMENT;
    }
    observation.connection_ready =
        PQstatus(postgresql_connection) == CONNECTION_OK;
    observation.ssl_in_use = PQsslInUse(postgresql_connection) != 0;
    observation.policy_applied =
        vps_libpq_policy_applied(postgresql_connection, policy);
    if (observation.ssl_in_use) {
        observation.ssl_library_reported =
            vps_libpq_attribute_reported(postgresql_connection, "library");
        observation.protocol_reported =
            vps_libpq_attribute_reported(postgresql_connection, "protocol");
        observation.cipher_reported =
            vps_libpq_attribute_reported(postgresql_connection, "cipher");
    }
    return vps_tls_policy_verify(policy, &observation, outcome, logger);
}
