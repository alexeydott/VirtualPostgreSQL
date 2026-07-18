#include "vps_metadata_cache.h"

#include <string.h>

#define VPS_CACHE_MAGIC UINT32_C(0x31535056)
#define VPS_CACHE_HEADER_BYTES 100U
#define VPS_CACHE_FIELD_BYTES 40U

static uint64_t vps_cache_hash(const unsigned char *bytes, size_t size)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    for (index = 0U; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void vps_cache_put16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8U);
}

static void vps_cache_put32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8U);
    p[2] = (unsigned char)(value >> 16U); p[3] = (unsigned char)(value >> 24U);
}

static void vps_cache_put64(unsigned char *p, uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index)
        p[index] = (unsigned char)(value >> (index * 8U));
}

static uint16_t vps_cache_get16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static uint32_t vps_cache_get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static uint64_t vps_cache_get64(const unsigned char *p)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index)
        value |= (uint64_t)p[index] << (index * 8U);
    return value;
}

VpsMetadataCacheResult vps_metadata_snapshot_init(
    VpsMetadataSnapshot *snapshot, const VpsAllocator *allocator)
{
    if (snapshot == NULL || !vps_allocator_is_valid(allocator))
        return VPS_METADATA_CACHE_INVALID_ARGUMENT;
    (void)memset(snapshot, 0, sizeof(*snapshot));
    snapshot->allocator = *allocator;
    if (vps_buffer_init(&snapshot->text, allocator,
                        VPS_METADATA_CACHE_MAX_BYTES) != VPS_MEMORY_OK)
        return VPS_METADATA_CACHE_OUT_OF_MEMORY;
    snapshot->initialized = 1;
    return VPS_METADATA_CACHE_OK;
}

VpsMetadataCacheResult vps_metadata_snapshot_add_field(
    VpsMetadataSnapshot *snapshot, const char *name, size_t name_length,
    const VpsMetadataCacheField *field)
{
    VpsMetadataCacheField copied;
    if (snapshot == NULL || !snapshot->initialized || field == NULL ||
        name == NULL || name_length == 0U || name_length > UINT16_MAX ||
        snapshot->field_count >= VPS_METADATA_CACHE_MAX_FIELDS ||
        memchr(name, '\0', name_length) != NULL)
        return VPS_METADATA_CACHE_INVALID_ARGUMENT;
    copied = *field;
    copied.name.offset = snapshot->text.size;
    copied.name.length = name_length;
    if (vps_buffer_append(&snapshot->text, name, name_length) != VPS_MEMORY_OK)
        return VPS_METADATA_CACHE_OUT_OF_MEMORY;
    snapshot->fields[snapshot->field_count++] = copied;
    return VPS_METADATA_CACHE_OK;
}

const char *vps_metadata_snapshot_field_name(
    const VpsMetadataSnapshot *snapshot, size_t index, size_t *length)
{
    const VpsMetadataCacheString *name;
    if (length != NULL) *length = 0U;
    if (snapshot == NULL || !snapshot->initialized ||
        index >= snapshot->field_count) return NULL;
    name = &snapshot->fields[index].name;
    if (name->offset > snapshot->text.size ||
        name->length > snapshot->text.size - name->offset) return NULL;
    if (length != NULL) *length = name->length;
    return (const char *)snapshot->text.data + name->offset;
}

VpsMetadataCacheResult vps_metadata_snapshot_validate(
    const VpsMetadataSnapshot *snapshot)
{
    size_t index;
    if (snapshot == NULL || !snapshot->initialized ||
        snapshot->field_count == 0U ||
        snapshot->field_count > VPS_METADATA_CACHE_MAX_FIELDS ||
        snapshot->visible_count == 0U ||
        snapshot->visible_count > snapshot->field_count ||
        snapshot->key_count > VPS_METADATA_CACHE_MAX_KEYS)
        return VPS_METADATA_CACHE_INVALID_FORMAT;
    for (index = 0U; index < snapshot->field_count; ++index) {
        const char *name;
        size_t length;
        size_t prior;
        name = vps_metadata_snapshot_field_name(snapshot, index, &length);
        if (name == NULL || length == 0U || memchr(name, '\0', length) != NULL)
            return VPS_METADATA_CACHE_INVALID_FORMAT;
        for (prior = 0U; prior < index; ++prior) {
            size_t prior_length;
            const char *prior_name = vps_metadata_snapshot_field_name(
                snapshot, prior, &prior_length);
            if (prior_length == length && memcmp(prior_name, name, length) == 0)
                return VPS_METADATA_CACHE_INVALID_FORMAT;
        }
    }
    for (index = 0U; index < snapshot->key_count; ++index)
        if (snapshot->key_columns[index] >= snapshot->visible_count)
            return VPS_METADATA_CACHE_INVALID_FORMAT;
    return VPS_METADATA_CACHE_OK;
}

VpsMetadataCacheResult vps_metadata_snapshot_encode(
    const VpsMetadataSnapshot *snapshot, VpsBuffer *encoded)
{
    size_t size = VPS_CACHE_HEADER_BYTES + 8U;
    size_t index;
    unsigned char bytes[VPS_CACHE_FIELD_BYTES];
    if (snapshot == NULL || encoded == NULL ||
        vps_metadata_snapshot_validate(snapshot) != VPS_METADATA_CACHE_OK)
        return VPS_METADATA_CACHE_INVALID_ARGUMENT;
    for (index = 0U; index < snapshot->field_count; ++index) {
        size_t name_length = snapshot->fields[index].name.length;
        if (size > VPS_METADATA_CACHE_MAX_BYTES - VPS_CACHE_FIELD_BYTES ||
            size + VPS_CACHE_FIELD_BYTES >
                VPS_METADATA_CACHE_MAX_BYTES - name_length)
            return VPS_METADATA_CACHE_LIMIT_EXCEEDED;
        size += VPS_CACHE_FIELD_BYTES + name_length;
    }
    if (size > VPS_METADATA_CACHE_MAX_BYTES - snapshot->key_count * 2U)
        return VPS_METADATA_CACHE_LIMIT_EXCEEDED;
    size += snapshot->key_count * 2U;
    if (vps_buffer_init(encoded, &snapshot->allocator,
                        VPS_METADATA_CACHE_MAX_BYTES) != VPS_MEMORY_OK ||
        vps_buffer_reserve(encoded, size) != VPS_MEMORY_OK)
        return VPS_METADATA_CACHE_OUT_OF_MEMORY;
    (void)memset(bytes, 0, sizeof(bytes));
    {
        unsigned char header[VPS_CACHE_HEADER_BYTES];
        (void)memset(header, 0, sizeof(header));
        vps_cache_put32(header, VPS_CACHE_MAGIC);
        vps_cache_put32(header + 4U, VPS_METADATA_CACHE_VERSION);
        vps_cache_put16(header + 8U, (uint16_t)snapshot->field_count);
        vps_cache_put16(header + 10U, (uint16_t)snapshot->visible_count);
        vps_cache_put16(header + 12U, (uint16_t)snapshot->key_count);
        vps_cache_put64(header + 16U, snapshot->source_fingerprint);
        vps_cache_put64(header + 24U, snapshot->layout_fingerprint);
        vps_cache_put64(header + 32U, snapshot->captured_at_ms);
        vps_cache_put64(header + 40U, snapshot->validated_at_ms);
        vps_cache_put64(header + 48U, snapshot->configuration_generation);
        vps_cache_put32(header + 56U, snapshot->relation_oid);
        vps_cache_put32(header + 60U, snapshot->spatial_namespace_oid);
        vps_cache_put32(header + 64U, snapshot->spatial_geometry_oid);
        vps_cache_put32(header + 68U, snapshot->spatial_geography_oid);
        vps_cache_put32(header + 72U, snapshot->spatial_flags);
        vps_cache_put32(header + 76U, snapshot->spatial_format);
        vps_cache_put32(header + 80U, (uint32_t)size);
        vps_cache_put64(header + 84U, snapshot->connection_identity_hash);
        vps_cache_put64(header + 92U, snapshot->relation_policy_fingerprint);
        if (vps_buffer_append(encoded, header, sizeof(header)) != VPS_MEMORY_OK)
            goto oom;
    }
    for (index = 0U; index < snapshot->field_count; ++index) {
        const VpsMetadataCacheField *field = &snapshot->fields[index];
        const char *name;
        size_t name_length;
        (void)memset(bytes, 0, sizeof(bytes));
        vps_cache_put32(bytes, field->type_oid);
        vps_cache_put32(bytes + 4U, (uint32_t)field->type_modifier);
        vps_cache_put32(bytes + 8U, field->origin_relation_oid);
        vps_cache_put32(bytes + 12U, (uint32_t)field->origin_attribute_number);
        vps_cache_put32(bytes + 16U, field->spatial_srid);
        bytes[20] = field->spatial_kind; bytes[21] = field->spatial_type;
        bytes[22] = field->spatial_dimensions;
        vps_cache_put16(bytes + 24U, (uint16_t)field->name.length);
        vps_cache_put32(bytes + 28U, field->collation_oid);
        vps_cache_put64(bytes + 32U, field->policy_fingerprint);
        name = vps_metadata_snapshot_field_name(snapshot, index, &name_length);
        if (vps_buffer_append(encoded, bytes, sizeof(bytes)) != VPS_MEMORY_OK ||
            vps_buffer_append(encoded, name, name_length) != VPS_MEMORY_OK)
            goto oom;
    }
    for (index = 0U; index < snapshot->key_count; ++index) {
        unsigned char key[2];
        vps_cache_put16(key, snapshot->key_columns[index]);
        if (vps_buffer_append(encoded, key, sizeof(key)) != VPS_MEMORY_OK)
            goto oom;
    }
    {
        unsigned char hash[8];
        vps_cache_put64(hash, vps_cache_hash(encoded->data, encoded->size));
        if (vps_buffer_append(encoded, hash, sizeof(hash)) != VPS_MEMORY_OK)
            goto oom;
    }
    return VPS_METADATA_CACHE_OK;
oom:
    vps_buffer_reset(encoded);
    return VPS_METADATA_CACHE_OUT_OF_MEMORY;
}

VpsMetadataCacheResult vps_metadata_snapshot_decode(
    VpsMetadataSnapshot *snapshot, const VpsAllocator *allocator,
    const void *encoded, size_t encoded_size)
{
    const unsigned char *bytes = (const unsigned char *)encoded;
    size_t offset = VPS_CACHE_HEADER_BYTES;
    size_t index;
    VpsMetadataSnapshot candidate;
    if (snapshot == NULL || allocator == NULL || bytes == NULL ||
        encoded_size < VPS_CACHE_HEADER_BYTES + 8U ||
        encoded_size > VPS_METADATA_CACHE_MAX_BYTES ||
        vps_cache_get32(bytes) != VPS_CACHE_MAGIC ||
        vps_cache_get32(bytes + 4U) != VPS_METADATA_CACHE_VERSION ||
        vps_cache_get32(bytes + 80U) != encoded_size ||
        vps_cache_hash(bytes, encoded_size - 8U) !=
            vps_cache_get64(bytes + encoded_size - 8U))
        return VPS_METADATA_CACHE_INVALID_FORMAT;
    if (vps_metadata_snapshot_init(&candidate, allocator) !=
        VPS_METADATA_CACHE_OK) return VPS_METADATA_CACHE_OUT_OF_MEMORY;
    candidate.field_count = 0U;
    candidate.visible_count = vps_cache_get16(bytes + 10U);
    candidate.key_count = vps_cache_get16(bytes + 12U);
    candidate.source_fingerprint = vps_cache_get64(bytes + 16U);
    candidate.layout_fingerprint = vps_cache_get64(bytes + 24U);
    candidate.captured_at_ms = vps_cache_get64(bytes + 32U);
    candidate.validated_at_ms = vps_cache_get64(bytes + 40U);
    candidate.configuration_generation = vps_cache_get64(bytes + 48U);
    candidate.relation_oid = vps_cache_get32(bytes + 56U);
    candidate.spatial_namespace_oid = vps_cache_get32(bytes + 60U);
    candidate.spatial_geometry_oid = vps_cache_get32(bytes + 64U);
    candidate.spatial_geography_oid = vps_cache_get32(bytes + 68U);
    candidate.spatial_flags = vps_cache_get32(bytes + 72U);
    candidate.spatial_format = vps_cache_get32(bytes + 76U);
    candidate.connection_identity_hash = vps_cache_get64(bytes + 84U);
    candidate.relation_policy_fingerprint = vps_cache_get64(bytes + 92U);
    if (vps_cache_get16(bytes + 8U) > VPS_METADATA_CACHE_MAX_FIELDS ||
        candidate.key_count > VPS_METADATA_CACHE_MAX_KEYS) goto invalid;
    for (index = 0U; index < vps_cache_get16(bytes + 8U); ++index) {
        VpsMetadataCacheField field;
        uint16_t name_length;
        if (offset > encoded_size - 8U ||
            VPS_CACHE_FIELD_BYTES > encoded_size - 8U - offset) goto invalid;
        (void)memset(&field, 0, sizeof(field));
        field.type_oid = vps_cache_get32(bytes + offset);
        field.type_modifier = (int32_t)vps_cache_get32(bytes + offset + 4U);
        field.origin_relation_oid = vps_cache_get32(bytes + offset + 8U);
        field.origin_attribute_number =
            (int32_t)vps_cache_get32(bytes + offset + 12U);
        field.spatial_srid = vps_cache_get32(bytes + offset + 16U);
        field.spatial_kind = bytes[offset + 20U];
        field.spatial_type = bytes[offset + 21U];
        field.spatial_dimensions = bytes[offset + 22U];
        name_length = vps_cache_get16(bytes + offset + 24U);
        field.collation_oid = vps_cache_get32(bytes + offset + 28U);
        field.policy_fingerprint = vps_cache_get64(bytes + offset + 32U);
        offset += VPS_CACHE_FIELD_BYTES;
        if (name_length == 0U || name_length > encoded_size - 8U - offset ||
            vps_metadata_snapshot_add_field(
                &candidate, (const char *)bytes + offset, name_length,
                &field) != VPS_METADATA_CACHE_OK) goto invalid;
        offset += name_length;
    }
    if (candidate.key_count * 2U > encoded_size - 8U - offset) goto invalid;
    for (index = 0U; index < candidate.key_count; ++index) {
        candidate.key_columns[index] = vps_cache_get16(bytes + offset);
        offset += 2U;
    }
    if (offset != encoded_size - 8U ||
        vps_metadata_snapshot_validate(&candidate) != VPS_METADATA_CACHE_OK)
        goto invalid;
    if (snapshot->initialized) vps_metadata_snapshot_reset(snapshot);
    *snapshot = candidate;
    return VPS_METADATA_CACHE_OK;
invalid:
    vps_metadata_snapshot_reset(&candidate);
    return VPS_METADATA_CACHE_INVALID_FORMAT;
}

VpsMetadataDrift vps_metadata_snapshot_compare(
    const VpsMetadataSnapshot *stored, const VpsMetadataSnapshot *live)
{
    size_t index;
    if (vps_metadata_snapshot_validate(stored) != VPS_METADATA_CACHE_OK ||
        vps_metadata_snapshot_validate(live) != VPS_METADATA_CACHE_OK ||
        stored->relation_oid != live->relation_oid ||
        stored->field_count != live->field_count ||
        stored->visible_count != live->visible_count ||
        stored->key_count != live->key_count ||
        stored->spatial_namespace_oid != live->spatial_namespace_oid ||
        stored->spatial_geometry_oid != live->spatial_geometry_oid ||
        stored->spatial_geography_oid != live->spatial_geography_oid ||
        stored->spatial_flags != live->spatial_flags ||
        stored->spatial_format != live->spatial_format ||
        stored->connection_identity_hash != live->connection_identity_hash ||
        stored->relation_policy_fingerprint !=
            live->relation_policy_fingerprint)
        return VPS_METADATA_DRIFT_INCOMPATIBLE;
    for (index = 0U; index < stored->field_count; ++index) {
        const VpsMetadataCacheField *a = &stored->fields[index];
        const VpsMetadataCacheField *b = &live->fields[index];
        size_t an, bn;
        const char *av = vps_metadata_snapshot_field_name(stored, index, &an);
        const char *bv = vps_metadata_snapshot_field_name(live, index, &bn);
        if (an != bn || memcmp(av, bv, an) != 0 ||
            a->type_oid != b->type_oid || a->type_modifier != b->type_modifier ||
            a->origin_relation_oid != b->origin_relation_oid ||
            a->origin_attribute_number != b->origin_attribute_number ||
            a->collation_oid != b->collation_oid ||
            a->policy_fingerprint != b->policy_fingerprint ||
            a->spatial_kind != b->spatial_kind ||
            a->spatial_type != b->spatial_type ||
            a->spatial_dimensions != b->spatial_dimensions ||
            a->spatial_srid != b->spatial_srid)
            return VPS_METADATA_DRIFT_INCOMPATIBLE;
    }
    for (index = 0U; index < stored->key_count; ++index)
        if (stored->key_columns[index] != live->key_columns[index])
            return VPS_METADATA_DRIFT_INCOMPATIBLE;
    if (stored->source_fingerprint == live->source_fingerprint &&
        stored->layout_fingerprint == live->layout_fingerprint)
        return VPS_METADATA_DRIFT_NONE;
    return VPS_METADATA_DRIFT_REFRESHABLE;
}

int vps_metadata_cache_fallback_allowed(VpsErrorClass error_class)
{
    return error_class == VPS_ERROR_CLASS_CONNECTION ||
           error_class == VPS_ERROR_CLASS_TIMEOUT;
}

void vps_metadata_snapshot_reset(VpsMetadataSnapshot *snapshot)
{
    if (snapshot == NULL) return;
    if (snapshot->initialized) vps_buffer_reset(&snapshot->text);
    (void)memset(snapshot, 0, sizeof(*snapshot));
}
