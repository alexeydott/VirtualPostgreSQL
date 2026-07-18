#include "vps_query_metadata.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char *const names[] = {"id", "payload", "created_at"};
    VpsAllocator allocator;
    VpsQueryDescribeField fields[3];
    VpsQueryDescribeResult described;
    VpsQueryMetadataPolicy policy;
    VpsQueryResultMetadata metadata;
    size_t index;
    if (data == NULL || size == 0U || size > VPS_QUERY_INDEXES_MAX_BYTES ||
        vps_allocator_system(&allocator) != VPS_MEMORY_OK)
        return 0;
    (void)memset(fields, 0, sizeof(fields));
    (void)memset(&described, 0, sizeof(described));
    (void)memset(&policy, 0, sizeof(policy));
    (void)memset(&metadata, 0, sizeof(metadata));
    for (index = 0U; index < 3U; ++index) {
        fields[index].name_length = strlen(names[index]);
        (void)memcpy(fields[index].name, names[index], fields[index].name_length + 1U);
        fields[index].type_oid = index == 0U ? 20U : 25U;
    }
    described.fields = fields;
    described.field_count = 3U;
    described.initialized = 1;
    policy.key_columns = (const char *)data;
    policy.key_columns_length = size;
    policy.query_indexes = (const char *)data;
    policy.query_indexes_length = size;
    if (vps_query_result_metadata_init(&metadata, &allocator, NULL) ==
        VPS_QUERY_METADATA_OK) {
        (void)vps_query_result_metadata_build(&metadata, &described, &policy);
        vps_query_result_metadata_cleanup(&metadata);
    }
    return 0;
}
