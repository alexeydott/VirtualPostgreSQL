#include "vps_libpq_client_conninfo.h"

#include <stdint.h>

static VpsConnectionStringResult vps_fuzz_consume(
    void *context, const VpsCredentialConfig *config)
{
    (void)context;
    (void)config;
    return VPS_CONNECTION_STRING_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (data != NULL && size != 0U && size <= VPS_ARGUMENT_VALUE_LIMIT)
        (void)vps_libpq_client_conninfo_parse(
            NULL, (const char *)data, size, vps_fuzz_consume, NULL);
    return 0;
}
