#include "vps_arguments.h"
#include "vps_platform.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    VpsAllocator allocator;
    VpsParsedArguments arguments;
    VpsArgumentsDiagnostic diagnostic;
    VpsArgumentInput input;
    if (data == NULL || size == 0U || size > VPS_ARGUMENT_VALUE_LIMIT)
        return 0;
    (void)memset(&arguments, 0, sizeof(arguments));
    if (vps_allocator_system(&allocator) != VPS_MEMORY_OK ||
        vps_arguments_init(&arguments, &allocator,
                           vps_platform_current_operations(), NULL) !=
            VPS_ARGUMENTS_OK)
        return 0;
    input.text = (const char *)data;
    input.length = size;
    (void)vps_arguments_parse(&arguments, &input, 1U, &diagnostic);
    (void)vps_arguments_reset(&arguments);
    return 0;
}
