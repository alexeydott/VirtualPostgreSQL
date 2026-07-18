#include "vps_metadata_cache.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { (void)fprintf(stderr, \
    "metadata_cache status=failed line=%d\n", __LINE__); return 0; } } while (0)

static int make_snapshot(VpsMetadataSnapshot *snapshot,
                         const VpsAllocator *allocator)
{
    VpsMetadataCacheField field;
    CHECK(vps_metadata_snapshot_init(snapshot, allocator) ==
          VPS_METADATA_CACHE_OK);
    snapshot->visible_count = 2U;
    snapshot->source_fingerprint = UINT64_C(0x1122334455667788);
    snapshot->layout_fingerprint = UINT64_C(0x8877665544332211);
    snapshot->captured_at_ms = UINT64_C(1000);
    snapshot->validated_at_ms = UINT64_C(2000);
    snapshot->configuration_generation = UINT64_C(3);
    snapshot->relation_oid = UINT32_C(42);
    snapshot->key_count = 1U;
    snapshot->key_columns[0] = 0U;
    (void)memset(&field, 0, sizeof(field));
    field.type_oid = 23U; field.type_modifier = -1;
    field.origin_relation_oid = 42U; field.origin_attribute_number = 1;
    CHECK(vps_metadata_snapshot_add_field(snapshot, "id", 2U, &field) ==
          VPS_METADATA_CACHE_OK);
    field.type_oid = 25U; field.origin_attribute_number = 2;
    CHECK(vps_metadata_snapshot_add_field(snapshot, "value", 5U, &field) ==
          VPS_METADATA_CACHE_OK);
    return 1;
}

static int test_round_trip_and_tamper(void)
{
    VpsAllocator allocator;
    VpsMetadataSnapshot source = {0};
    VpsMetadataSnapshot decoded = {0};
    VpsBuffer encoded = {0};
    VpsBuffer second = {0};
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    CHECK(make_snapshot(&source, &allocator));
    CHECK(vps_metadata_snapshot_encode(&source, &encoded) ==
          VPS_METADATA_CACHE_OK && encoded.size <= VPS_METADATA_CACHE_MAX_BYTES);
    CHECK(vps_metadata_snapshot_decode(&decoded, &allocator, encoded.data,
                                       encoded.size) == VPS_METADATA_CACHE_OK);
    CHECK(vps_metadata_snapshot_compare(&source, &decoded) ==
          VPS_METADATA_DRIFT_NONE);
    CHECK(vps_metadata_snapshot_encode(&decoded, &second) ==
          VPS_METADATA_CACHE_OK && second.size == encoded.size &&
          memcmp(second.data, encoded.data, encoded.size) == 0);
    encoded.data[20] ^= 1U;
    CHECK(vps_metadata_snapshot_decode(&decoded, &allocator, encoded.data,
                                       encoded.size) ==
          VPS_METADATA_CACHE_INVALID_FORMAT);
    vps_buffer_reset(&second); vps_buffer_reset(&encoded);
    vps_metadata_snapshot_reset(&decoded);
    vps_metadata_snapshot_reset(&source);
    return 1;
}

static int test_drift_and_fault(void)
{
    VpsAllocator system_allocator;
    VpsAllocator fault_allocator;
    VpsFaultAllocator fault;
    VpsMetadataSnapshot a = {0};
    VpsMetadataSnapshot b = {0};
    VpsBuffer encoded = {0};
    CHECK(vps_allocator_system(&system_allocator) == VPS_MEMORY_OK);
    CHECK(make_snapshot(&a, &system_allocator));
    CHECK(make_snapshot(&b, &system_allocator));
    b.source_fingerprint += 1U;
    CHECK(vps_metadata_snapshot_compare(&a, &b) ==
          VPS_METADATA_DRIFT_REFRESHABLE);
    b.fields[1].type_oid = 17U;
    CHECK(vps_metadata_snapshot_compare(&a, &b) ==
          VPS_METADATA_DRIFT_INCOMPATIBLE);
    b.fields[1].type_oid = a.fields[1].type_oid;
    b.fields[1].policy_fingerprint = 1U;
    CHECK(vps_metadata_snapshot_compare(&a, &b) ==
          VPS_METADATA_DRIFT_INCOMPATIBLE);
    b.fields[1].policy_fingerprint = a.fields[1].policy_fingerprint;
    b.relation_policy_fingerprint = 1U;
    CHECK(vps_metadata_snapshot_compare(&a, &b) ==
          VPS_METADATA_DRIFT_INCOMPATIBLE);
    CHECK(vps_metadata_cache_fallback_allowed(VPS_ERROR_CLASS_CONNECTION));
    CHECK(vps_metadata_cache_fallback_allowed(VPS_ERROR_CLASS_TIMEOUT));
    CHECK(!vps_metadata_cache_fallback_allowed(VPS_ERROR_CLASS_AUTH));
    CHECK(!vps_metadata_cache_fallback_allowed(VPS_ERROR_CLASS_TLS));
    CHECK(!vps_metadata_cache_fallback_allowed(VPS_ERROR_CLASS_CONFIG));
    CHECK(!vps_metadata_cache_fallback_allowed(VPS_ERROR_CLASS_SCHEMA));
    CHECK(!vps_metadata_cache_fallback_allowed(VPS_ERROR_CLASS_SQL));
    CHECK(vps_fault_allocator_init(&fault, &system_allocator, 1U) ==
          VPS_MEMORY_OK &&
          vps_fault_allocator_make(&fault, &fault_allocator) == VPS_MEMORY_OK);
    vps_metadata_snapshot_reset(&b);
    CHECK(vps_metadata_snapshot_init(&b, &fault_allocator) ==
          VPS_METADATA_CACHE_OK);
    b.visible_count = 1U;
    {
        VpsMetadataCacheField field = {0};
        field.type_oid = 23U;
        CHECK(vps_metadata_snapshot_add_field(&b, "id", 2U, &field) ==
              VPS_METADATA_CACHE_OUT_OF_MEMORY);
    }
    CHECK(vps_fault_allocator_reset(&fault, 0U) == VPS_MEMORY_OK);
    vps_buffer_reset(&encoded);
    vps_metadata_snapshot_reset(&b); vps_metadata_snapshot_reset(&a);
    return 1;
}

int main(void)
{
    if (!test_round_trip_and_tamper() || !test_drift_and_fault()) return 1;
    (void)printf("metadata_cache status=passed\n");
    return 0;
}
