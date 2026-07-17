#include "virtualpostgresql/vps_api.h"
#include "vps_private_api.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

static int vps_expect(int condition, const char *check_name)
{
    if (!condition) {
        (void)fprintf(stderr, "[abi] level=error check=%s status=failed\n",
                      check_name);
        return 0;
    }
    return 1;
}

int main(void)
{
    VpsAbiHeader header = {
        (uint32_t)sizeof(VpsAbiHeader),
        VPS_API_VERSION,
        UINT64_C(0)
    };
    uint32_t incompatible_version = VPS_API_VERSION_ENCODE(2, 0, 0);
    int passed = 1;

    passed &= vps_expect(sizeof(void *) == 4 || sizeof(void *) == 8,
                         "supported_pointer_width");
    passed &= vps_expect(offsetof(VpsCredentialConfig, header) == 0,
                         "config_header_prefix");
    passed &= vps_expect(offsetof(VpsCredentialLease, header) == 0,
                         "lease_header_prefix");
    passed &= vps_expect(offsetof(VpsCredentialProvider, header) == 0,
                         "provider_header_prefix");
    passed &= vps_expect(virtualpostgresql_api_version() == VPS_API_VERSION,
                         "api_version_function");
    passed &= vps_expect(
        virtualpostgresql_credential_config_structure_size() ==
            sizeof(VpsCredentialConfig),
        "config_structure_size_function");
    passed &= vps_expect(
        virtualpostgresql_credential_lease_structure_size() ==
            sizeof(VpsCredentialLease),
        "lease_structure_size_function");
    passed &= vps_expect(
        virtualpostgresql_credential_provider_structure_size() ==
            sizeof(VpsCredentialProvider),
        "provider_structure_size_function");
    passed &= vps_expect(
        vps_abi_validate_header(&header, (uint32_t)sizeof(header),
                                VPS_API_VERSION) == VPS_ABI_VALID,
        "compatible_header");

    header.structure_size -= UINT32_C(1);
    passed &= vps_expect(
        vps_abi_validate_header(&header, (uint32_t)sizeof(VpsAbiHeader),
                                VPS_API_VERSION) ==
            VPS_ABI_STRUCTURE_TOO_SMALL,
        "short_structure_rejected");
    header.structure_size = (uint32_t)sizeof(header);
    header.api_version = incompatible_version;
    passed &= vps_expect(
        vps_abi_validate_header(&header, (uint32_t)sizeof(VpsAbiHeader),
                                VPS_API_VERSION) ==
            VPS_ABI_VERSION_INCOMPATIBLE,
        "incompatible_major_rejected");
    passed &= vps_expect(
        vps_abi_validate_header(NULL, (uint32_t)sizeof(VpsAbiHeader),
                                VPS_API_VERSION) == VPS_ABI_NULL,
        "null_header_rejected");

    (void)printf(
        "[abi] level=info version=%" PRIu32
        " pointer_bits=%zu config_size=%zu lease_size=%zu provider_size=%zu "
        "status=%s\n",
        VPS_API_VERSION, sizeof(void *) * 8, sizeof(VpsCredentialConfig),
        sizeof(VpsCredentialLease), sizeof(VpsCredentialProvider),
        passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
