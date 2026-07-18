#include "vps_query_indexes.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char *const names[] = {"id", "name", "amount", "created_at"};
    VpsQueryDescribeField fields[4];
    VpsQueryDescribeResult described;
    VpsQueryIndexSet indexes;
    size_t index;
    if (data == NULL || size == 0U || size > VPS_QUERY_INDEXES_MAX_BYTES)
        return 0;
    (void)memset(&described, 0, sizeof(described));
    (void)memset(fields, 0, sizeof(fields));
    for (index = 0U; index < 4U; ++index) {
        fields[index].name_length = strlen(names[index]);
        (void)memcpy(fields[index].name, names[index], fields[index].name_length + 1U);
    }
    described.fields = fields;
    described.field_count = 4U;
    described.initialized = 1;
    (void)vps_query_indexes_parse((const char *)data, size, &described, &indexes);
    return 0;
}
