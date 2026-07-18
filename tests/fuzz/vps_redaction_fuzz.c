#include "vps_logging.h"

#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    VpsLogEvent event;
    char storage[VPS_LOG_MAX_STRING_LENGTH + 1U];
    if (data == NULL || size == 0U || size > VPS_LOG_MAX_SQL_TEXT_LENGTH)
        return 0;
    if (vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) == VPS_LOG_OK)
        (void)vps_log_event_add_primary_message(
            &event, (const char *)data, size, storage, sizeof(storage));
    return 0;
}
