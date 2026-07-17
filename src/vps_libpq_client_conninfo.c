#include "vps_libpq_client_conninfo.h"

#include "virtualpostgresql/vps_api.h"

#include <libpq-fe.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct VpsConninfoKeyword {
    const char *name;
    VpsCredentialFields field;
    size_t offset;
} VpsConninfoKeyword;

#define VPS_CONFIG_OFFSET(member) offsetof(VpsCredentialConfig, member)

static const VpsConninfoKeyword vps_conninfo_keywords[] = {
    {"host", VPS_CREDENTIAL_FIELD_HOSTS, VPS_CONFIG_OFFSET(hosts)},
    {"port", VPS_CREDENTIAL_FIELD_PORTS, VPS_CONFIG_OFFSET(ports)},
    {"user", VPS_CREDENTIAL_FIELD_USER, VPS_CONFIG_OFFSET(user)},
    {"password", VPS_CREDENTIAL_FIELD_PASSWORD, VPS_CONFIG_OFFSET(password)},
    {"dbname", VPS_CREDENTIAL_FIELD_DBNAME, VPS_CONFIG_OFFSET(dbname)},
    {"service", VPS_CREDENTIAL_FIELD_SERVICE, VPS_CONFIG_OFFSET(service)},
    {"servicefile", VPS_CREDENTIAL_FIELD_SERVICE_FILE,
     VPS_CONFIG_OFFSET(service_file)},
    {"sslmode", VPS_CREDENTIAL_FIELD_SSLMODE, VPS_CONFIG_OFFSET(sslmode)},
    {"sslrootcert", VPS_CREDENTIAL_FIELD_SSLROOTCERT,
     VPS_CONFIG_OFFSET(sslrootcert)},
    {"sslcert", VPS_CREDENTIAL_FIELD_SSLCERT, VPS_CONFIG_OFFSET(sslcert)},
    {"sslkey", VPS_CREDENTIAL_FIELD_SSLKEY, VPS_CONFIG_OFFSET(sslkey)},
    {"sslcrl", VPS_CREDENTIAL_FIELD_SSLCRL, VPS_CONFIG_OFFSET(sslcrl)},
    {"channel_binding", VPS_CREDENTIAL_FIELD_CHANNEL_BINDING,
     VPS_CONFIG_OFFSET(channel_binding)},
    {"target_session_attrs", VPS_CREDENTIAL_FIELD_TARGET_SESSION_ATTRS,
     VPS_CONFIG_OFFSET(target_session_attrs)},
    {"connect_timeout", VPS_CREDENTIAL_FIELD_CONNECT_TIMEOUT,
     VPS_CONFIG_OFFSET(connect_timeout)},
    {"application_name", VPS_CREDENTIAL_FIELD_APPLICATION_NAME,
     VPS_CONFIG_OFFSET(application_name)},
    {"fallback_application_name", VPS_CREDENTIAL_FIELD_APPLICATION_NAME,
     VPS_CONFIG_OFFSET(application_name)}};

static const VpsConninfoKeyword *vps_find_keyword(const char *name,
                                                   size_t length)
{
    size_t index;
    for (index = 0U; index < sizeof(vps_conninfo_keywords) /
                                  sizeof(vps_conninfo_keywords[0]); ++index) {
        if (strlen(vps_conninfo_keywords[index].name) == length &&
            memcmp(vps_conninfo_keywords[index].name, name, length) == 0) {
            return &vps_conninfo_keywords[index];
        }
    }
    return NULL;
}

static int vps_scan_keywords(const char *text, size_t length)
{
    size_t cursor = 0U;
    VpsCredentialFields seen = 0U;

    while (cursor < length) {
        size_t start;
        const VpsConninfoKeyword *keyword;
        int quoted = 0;

        while (cursor < length && (text[cursor] == ' ' || text[cursor] == '\t' ||
                                   text[cursor] == '\r' || text[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor == length) {
            break;
        }
        start = cursor;
        while (cursor < length &&
               ((text[cursor] >= 'a' && text[cursor] <= 'z') ||
                (text[cursor] >= 'A' && text[cursor] <= 'Z') ||
                text[cursor] == '_')) {
            ++cursor;
        }
        keyword = vps_find_keyword(text + start, cursor - start);
        if (keyword == NULL || cursor == start || cursor == length ||
            text[cursor] != '=' || (seen & keyword->field) != 0U) {
            return 0;
        }
        seen |= keyword->field;
        ++cursor;
        if (cursor < length && text[cursor] == '\'') {
            quoted = 1;
            ++cursor;
        }
        while (cursor < length) {
            if (text[cursor] == '\\') {
                if (++cursor == length) {
                    return 0;
                }
                ++cursor;
            } else if (quoted && text[cursor] == '\'') {
                ++cursor;
                quoted = 0;
                break;
            } else if (!quoted && (text[cursor] == ' ' || text[cursor] == '\t' ||
                                   text[cursor] == '\r' || text[cursor] == '\n')) {
                break;
            } else {
                ++cursor;
            }
        }
        if (quoted) {
            return 0;
        }
        if (cursor < length && text[cursor] != ' ' && text[cursor] != '\t' &&
            text[cursor] != '\r' && text[cursor] != '\n') {
            return 0;
        }
    }
    return seen != 0U;
}

static void vps_erase_bytes(char *value)
{
    volatile unsigned char *bytes = (volatile unsigned char *)value;
    size_t length;
    size_t index;
    if (value == NULL) {
        return;
    }
    length = strlen(value);
    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void vps_release_options(PQconninfoOption *options)
{
    PQconninfoOption *option;
    if (options == NULL) {
        return;
    }
    for (option = options; option->keyword != NULL; ++option) {
        vps_erase_bytes(option->val);
    }
    PQconninfoFree(options);
}

VpsConnectionStringResult vps_libpq_client_conninfo_parse(
    void *context,
    const char *conninfo,
    size_t conninfo_length,
    VpsConninfoConsumer consumer,
    void *consumer_context)
{
    char *copy;
    char *error = NULL;
    PQconninfoOption *parsed;
    PQconninfoOption *option;
    VpsCredentialConfig config;
    VpsConnectionStringResult result;

    (void)context;
    if (conninfo == NULL || consumer == NULL || conninfo_length == 0U ||
        conninfo_length > VPS_ARGUMENT_VALUE_LIMIT ||
        memchr(conninfo, '\0', conninfo_length) != NULL ||
        !vps_scan_keywords(conninfo, conninfo_length)) {
        return VPS_CONNECTION_STRING_CONNINFO_REJECTED;
    }
    copy = (char *)malloc(conninfo_length + 1U);
    if (copy == NULL) {
        return VPS_CONNECTION_STRING_OUT_OF_MEMORY;
    }
    (void)memcpy(copy, conninfo, conninfo_length);
    copy[conninfo_length] = '\0';
    parsed = PQconninfoParse(copy, &error);
    (void)memset(copy, 0, conninfo_length + 1U);
    free(copy);
    if (parsed == NULL) {
        if (error != NULL) {
            vps_erase_bytes(error);
            PQfreemem(error);
        }
        return VPS_CONNECTION_STRING_CONNINFO_REJECTED;
    }
    (void)memset(&config, 0, sizeof(config));
    config.header.structure_size = (uint32_t)sizeof(config);
    config.header.api_version = VPS_API_VERSION;
    for (option = parsed; option->keyword != NULL; ++option) {
        const VpsConninfoKeyword *keyword;
        const char **member;
        if (option->val == NULL) {
            continue;
        }
        keyword = vps_find_keyword(option->keyword, strlen(option->keyword));
        if (keyword == NULL) {
            continue;
        }
        if (strcmp(option->keyword, "fallback_application_name") == 0 &&
            strcmp(option->val, "VirtualPostgreSQL") != 0) {
            vps_release_options(parsed);
            return VPS_CONNECTION_STRING_CONNINFO_REJECTED;
        }
        member = (const char **)((unsigned char *)&config + keyword->offset);
        *member = option->val;
        config.header.present_fields |= keyword->field;
    }
    result = consumer(consumer_context, &config);
    vps_release_options(parsed);
    return result;
}
