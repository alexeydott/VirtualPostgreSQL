#include <virtualpostgresql/vps_api.h>

#include <string.h>

static int32_t VPS_CALL resolve(void *context, const char *reference,
                                uint32_t length, VpsCredentialLease *lease)
{
    VpsCredentialConfig *config = (VpsCredentialConfig *)context;
    if (length != 13U || memcmp(reference, "app/reporting", 13U) != 0) {
        return VPS_CREDENTIAL_PROVIDER_NOT_FOUND;
    }
    lease->header.structure_size = (uint32_t)sizeof(*lease);
    lease->header.api_version = VPS_API_VERSION;
    lease->header.present_fields = VPS_CREDENTIAL_LEASE_FIELDS_CURRENT;
    lease->config = config;
    lease->provider_lease = NULL;
    return VPS_CREDENTIAL_PROVIDER_OK;
}

static void VPS_CALL release(void *context, VpsCredentialLease *lease)
{
    (void)context;
    (void)lease;
    /* Release provider-owned state here; never log config strings. */
}

void make_provider(VpsCredentialProvider *provider,
                   VpsCredentialConfig *provider_owned_config)
{
    provider->header.structure_size = (uint32_t)sizeof(*provider);
    provider->header.api_version = VPS_API_VERSION;
    provider->header.present_fields = VPS_CREDENTIAL_PROVIDER_FIELDS_CURRENT;
    provider->resolve = resolve;
    provider->release = release;
    provider->provider_context = provider_owned_config;
}
