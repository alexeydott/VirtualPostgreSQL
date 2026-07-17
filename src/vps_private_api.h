#ifndef VPS_PRIVATE_API_H
#define VPS_PRIVATE_API_H

#include <stdint.h>

#include "virtualpostgresql/vps_api.h"

#define VPS_PRIVATE_API_VERSION UINT32_C(1)

typedef enum VpsAbiValidationResult {
    VPS_ABI_VALID = 0,
    VPS_ABI_NULL = 1,
    VPS_ABI_STRUCTURE_TOO_SMALL = 2,
    VPS_ABI_VERSION_INCOMPATIBLE = 3
} VpsAbiValidationResult;

VpsAbiValidationResult vps_abi_validate_header(
    const VpsAbiHeader *header,
    uint32_t minimum_structure_size,
    uint32_t supported_api_version);

#endif
