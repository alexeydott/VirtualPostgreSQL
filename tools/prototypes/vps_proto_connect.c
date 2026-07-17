#include "vps_proto_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum vps_connect_mode {
    VPS_MODE_SUCCESS,
    VPS_MODE_AUTH_FAILURE,
    VPS_MODE_REFUSED,
    VPS_MODE_TIMEOUT,
    VPS_MODE_INTERRUPT,
    VPS_MODE_PARTIAL
} vps_connect_mode;

static const char *vps_required_environment(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : NULL;
}

static int vps_parse_mode(const char *text, vps_connect_mode *mode)
{
    if (strcmp(text, "success") == 0) { *mode = VPS_MODE_SUCCESS; return 1; }
    if (strcmp(text, "auth-failure") == 0) { *mode = VPS_MODE_AUTH_FAILURE; return 1; }
    if (strcmp(text, "refused") == 0) { *mode = VPS_MODE_REFUSED; return 1; }
    if (strcmp(text, "timeout") == 0) { *mode = VPS_MODE_TIMEOUT; return 1; }
    if (strcmp(text, "interrupt") == 0) { *mode = VPS_MODE_INTERRUPT; return 1; }
    if (strcmp(text, "partial") == 0) { *mode = VPS_MODE_PARTIAL; return 1; }
    return 0;
}

static char *vps_make_bad_password(const char *password)
{
    size_t length;
    char *bad_password;

    if (password == NULL) {
        return NULL;
    }
    length = strlen(password);
    bad_password = (char *)malloc(length + 2U);
    if (bad_password == NULL) {
        return NULL;
    }
    memcpy(bad_password, password, length + 1U);
    if (length == 0U) {
        bad_password[0] = 'x';
        bad_password[1] = '\0';
    } else {
        bad_password[0] = bad_password[0] == 'x' ? 'y' : 'x';
    }
    return bad_password;
}

int main(int argc, char **argv)
{
    vps_connect_mode mode;
    vps_connect_config config;
    vps_connect_outcome outcome;
    vps_connect_result expected;
    HANDLE interrupt_event = NULL;
    char *bad_password = NULL;
    unsigned int finish_count = 0U;
    char fields[256];
    unsigned long timeout_ms = 3000UL;
    char *end = NULL;

    if (argc < 2 || !vps_parse_mode(argv[1], &mode)) {
        vps_log("error", "usage", "modes=success,auth-failure,refused,timeout,interrupt,partial");
        return 64;
    }
    if (argc >= 3) {
        timeout_ms = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || timeout_ms == 0UL || timeout_ms > 60000UL) {
            vps_log("error", "invalid_timeout", NULL);
            return 65;
        }
    }

    config.host = vps_required_environment("VPS_TEST_HOST");
    config.port = vps_required_environment("VPS_TEST_PORT");
    config.user = vps_required_environment("VPS_TEST_USER");
    config.password = vps_required_environment("VPS_TEST_PASSWORD");
    config.database = vps_required_environment("VPS_TEST_DATABASE");
    config.sslmode = vps_required_environment("VPS_TEST_SSLMODE");
    if (mode == VPS_MODE_REFUSED || mode == VPS_MODE_TIMEOUT || mode == VPS_MODE_INTERRUPT || mode == VPS_MODE_PARTIAL) {
        config.host = "127.0.0.1";
        config.port = mode == VPS_MODE_REFUSED ? vps_required_environment("VPS_TEST_REFUSED_PORT") : vps_required_environment("VPS_TEST_STALL_PORT");
        config.user = "vps_probe";
        config.password = NULL;
        config.database = "vps_probe";
        config.sslmode = "disable";
    }
    if (mode == VPS_MODE_AUTH_FAILURE) {
        bad_password = vps_make_bad_password(config.password);
        if (bad_password == NULL) {
            vps_log("error", "missing_test_configuration", "field=password");
            return 66;
        }
        config.password = bad_password;
    }
    if (config.host == NULL || config.port == NULL || config.user == NULL || config.database == NULL || config.sslmode == NULL) {
        free(bad_password);
        vps_log("error", "missing_test_configuration", NULL);
        return 66;
    }
    if ((mode == VPS_MODE_SUCCESS || mode == VPS_MODE_AUTH_FAILURE || mode == VPS_MODE_PARTIAL) && strcmp(config.sslmode, "disable") != 0) {
        free(bad_password);
        vps_log("error", "unsafe_test_sslmode", "required=disable");
        return 67;
    }

    if (mode == VPS_MODE_INTERRUPT || mode == VPS_MODE_PARTIAL) {
        interrupt_event = CreateEventW(NULL, TRUE, TRUE, NULL);
        if (interrupt_event == NULL) {
            free(bad_password);
            vps_log("error", "interrupt_event_failed", NULL);
            return 68;
        }
    }

    (void)vps_connect_async(&config, (uint32_t)timeout_ms, interrupt_event, &outcome);
    if (outcome.connection != NULL) {
        PQfinish(outcome.connection);
        outcome.connection = NULL;
        finish_count++;
    }
    if (interrupt_event != NULL) {
        CloseHandle(interrupt_event);
    }
    free(bad_password);

    expected = VPS_CONNECT_OK;
    if (mode == VPS_MODE_AUTH_FAILURE || mode == VPS_MODE_REFUSED) { expected = VPS_CONNECT_FAILED; }
    if (mode == VPS_MODE_TIMEOUT) { expected = VPS_CONNECT_TIMEOUT; }
    if (mode == VPS_MODE_INTERRUPT || mode == VPS_MODE_PARTIAL) { expected = VPS_CONNECT_INTERRUPTED; }
    (void)snprintf(fields, sizeof(fields),
                   "result=%s poll=%s duration_ms=%llu polls=%u waits=%u finish_count=%u",
                   vps_connect_result_name(outcome.result),
                   vps_poll_name(outcome.last_poll),
                   (unsigned long long)outcome.duration_ms,
                   outcome.poll_count,
                   outcome.wait_count,
                   finish_count);
    vps_log(outcome.result == expected && finish_count == 1U ? "info" : "error", "connect_complete", fields);
    return outcome.result == expected && finish_count == 1U ? 0 : 1;
}
