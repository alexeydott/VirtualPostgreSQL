#include "virtualpostgresql/vps_api.h"
#include "vps_private_api.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

_Static_assert(sizeof(VpsAbiHeader) == 16U, "VpsAbiHeader ABI 1.0 size");
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(VpsCredentialConfig) == 112U,
               "VpsCredentialConfig Win32 ABI 1.0 size");
_Static_assert(sizeof(VpsCredentialLease) == 40U,
               "VpsCredentialLease Win32 ABI 1.0 size");
_Static_assert(sizeof(VpsCredentialProvider) == 48U,
               "VpsCredentialProvider Win32 ABI 1.0 size");
_Static_assert(sizeof(VpsQueryProfileLease) == 56U,
               "VpsQueryProfileLease Win32 ABI size");
_Static_assert(sizeof(VpsQueryProfileProvider) == 48U,
               "VpsQueryProfileProvider Win32 ABI size");
#elif UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(VpsCredentialConfig) == 200U,
               "VpsCredentialConfig x64 ABI 1.0 size");
_Static_assert(sizeof(VpsCredentialLease) == 64U,
               "VpsCredentialLease x64 ABI 1.0 size");
_Static_assert(sizeof(VpsCredentialProvider) == 72U,
               "VpsCredentialProvider x64 ABI 1.0 size");
_Static_assert(sizeof(VpsQueryProfileLease) == 80U,
               "VpsQueryProfileLease x64 ABI size");
_Static_assert(sizeof(VpsQueryProfileProvider) == 72U,
               "VpsQueryProfileProvider x64 ABI size");
#else
#error Unsupported Windows 1.0 pointer width
#endif
_Static_assert(offsetof(VpsCredentialConfig, header) == 0U,
               "VpsCredentialConfig ABI header prefix");
_Static_assert(offsetof(VpsCredentialLease, header) == 0U,
               "VpsCredentialLease ABI header prefix");
_Static_assert(offsetof(VpsCredentialProvider, header) == 0U,
               "VpsCredentialProvider ABI header prefix");
_Static_assert(offsetof(VpsQueryProfileLease, header) == 0U,
               "VpsQueryProfileLease ABI header prefix");
_Static_assert(offsetof(VpsQueryProfileProvider, header) == 0U,
               "VpsQueryProfileProvider ABI header prefix");

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
    volatile size_t pointer_size = sizeof(void *);
    volatile size_t config_header_offset = offsetof(VpsCredentialConfig, header);
    volatile size_t lease_header_offset = offsetof(VpsCredentialLease, header);
    volatile size_t provider_header_offset = offsetof(VpsCredentialProvider, header);
    volatile size_t query_lease_header_offset =
        offsetof(VpsQueryProfileLease, header);
    volatile size_t query_provider_header_offset =
        offsetof(VpsQueryProfileProvider, header);
    int passed = 1;

    passed &= vps_expect(pointer_size == 4U || pointer_size == 8U,
                         "supported_pointer_width");
    passed &= vps_expect(config_header_offset == 0U,
                         "config_header_prefix");
    passed &= vps_expect(lease_header_offset == 0U,
                         "lease_header_prefix");
    passed &= vps_expect(provider_header_offset == 0U,
                         "provider_header_prefix");
    passed &= vps_expect(query_lease_header_offset == 0U,
                         "query_lease_header_prefix");
    passed &= vps_expect(query_provider_header_offset == 0U,
                         "query_provider_header_prefix");
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
        virtualpostgresql_query_profile_lease_structure_size() ==
            sizeof(VpsQueryProfileLease),
        "query_profile_lease_structure_size_function");
    passed &= vps_expect(
        virtualpostgresql_query_profile_provider_structure_size() ==
            sizeof(VpsQueryProfileProvider),
        "query_profile_provider_structure_size_function");
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
        "query_lease_size=%zu query_provider_size=%zu "
        "status=%s\n",
        VPS_API_VERSION, sizeof(void *) * 8, sizeof(VpsCredentialConfig),
        sizeof(VpsCredentialLease), sizeof(VpsCredentialProvider),
        sizeof(VpsQueryProfileLease), sizeof(VpsQueryProfileProvider),
        passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
