#include "vps_private_api.h"

#include <limits.h>
#include <stddef.h>

_Static_assert(sizeof(uint32_t) * CHAR_BIT == 32, "uint32_t must be 32 bits");
_Static_assert(sizeof(uint64_t) * CHAR_BIT == 64, "uint64_t must be 64 bits");
_Static_assert(offsetof(VpsAbiHeader, structure_size) == 0,
               "structure_size must be the first ABI field");
_Static_assert(offsetof(VpsAbiHeader, api_version) == sizeof(uint32_t),
               "api_version must follow structure_size");

uint32_t VPS_CALL virtualpostgresql_api_version(void)
{
    return VPS_API_VERSION;
}

uint32_t VPS_CALL virtualpostgresql_credential_config_structure_size(void)
{
    return (uint32_t)sizeof(VpsCredentialConfig);
}

uint32_t VPS_CALL virtualpostgresql_credential_lease_structure_size(void)
{
    return (uint32_t)sizeof(VpsCredentialLease);
}

uint32_t VPS_CALL virtualpostgresql_credential_provider_structure_size(void)
{
    return (uint32_t)sizeof(VpsCredentialProvider);
}

uint32_t VPS_CALL virtualpostgresql_query_profile_lease_structure_size(void)
{
    return (uint32_t)sizeof(VpsQueryProfileLease);
}

uint32_t VPS_CALL virtualpostgresql_query_profile_provider_structure_size(void)
{
    return (uint32_t)sizeof(VpsQueryProfileProvider);
}

VpsAbiValidationResult vps_abi_validate_header(
    const VpsAbiHeader *header,
    uint32_t minimum_structure_size,
    uint32_t supported_api_version)
{
    uint32_t supplied_major;
    uint32_t supported_major;

    if (header == NULL) {
        return VPS_ABI_NULL;
    }
    if (header->structure_size < minimum_structure_size) {
        return VPS_ABI_STRUCTURE_TOO_SMALL;
    }

    supplied_major = (header->api_version >> 24) & UINT32_C(0xff);
    supported_major = (supported_api_version >> 24) & UINT32_C(0xff);
    if (supplied_major != supported_major) {
        return VPS_ABI_VERSION_INCOMPATIBLE;
    }
    return VPS_ABI_VALID;
}
