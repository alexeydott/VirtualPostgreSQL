#include "vps_query_fingerprint.h"

#include <stdio.h>
#include <string.h>

static int failures=0;
#define CHECK(condition) do { if(!(condition)){ \
 (void)fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#condition); \
 ++failures;} } while(0)

int main(void)
{
    VpsAllocator allocator;
    VpsQueryResultMetadata metadata;
    VpsQueryResultColumn column;
    VpsQueryFingerprintInput input;
    VpsQueryFingerprint base;
    VpsQueryFingerprint changed;
    CHECK(vps_allocator_system(&allocator)==VPS_MEMORY_OK);
    (void)memset(&metadata,0,sizeof(metadata));
    metadata.allocator=allocator; metadata.columns=&column;
    metadata.column_count=1U; metadata.initialized=1;
    metadata.indexes.format_version=VPS_QUERY_INDEXES_FORMAT_VERSION;
    (void)memset(&column,0,sizeof(column));
    (void)memcpy(column.name,"id",3U); column.name_length=2U;
    column.canonical_name_hash=11U; column.type_oid=23U;
    column.type_modifier=-1;
    (void)memset(&input,0,sizeof(input)); input.normalized_query_hash=101U;
    input.metadata=&metadata;
    input.wrapper_format_version=VPS_QUERY_WRAPPER_FORMAT_VERSION;
    input.codec_registry_version=1U;
    CHECK(vps_query_fingerprint_build(&input,NULL,&base)==
          VPS_QUERY_METADATA_OK);
    CHECK(strlen(base.hex)==VPS_QUERY_FINGERPRINT_HEX_LENGTH);
    CHECK(vps_query_fingerprint_build(&input,NULL,&changed)==
          VPS_QUERY_METADATA_OK);
    CHECK(vps_query_fingerprint_compare(&base,&changed)==VPS_QUERY_CHANGE_NONE);
    input.profile_version=2U;
    CHECK(vps_query_fingerprint_build(&input,NULL,&changed)==
          VPS_QUERY_METADATA_OK);
    CHECK(vps_query_fingerprint_compare(&base,&changed)==
          VPS_QUERY_CHANGE_PROFILE);
    input.profile_version=0U; column.collation_oid=100U;
    CHECK(vps_query_fingerprint_build(&input,NULL,&changed)==
          VPS_QUERY_METADATA_OK);
    CHECK(vps_query_fingerprint_compare(&base,&changed)==
          VPS_QUERY_CHANGE_LAYOUT);
    column.collation_oid=0U; metadata.materialization=
        VPS_QUERY_MATERIALIZATION_TEMP;
    CHECK(vps_query_fingerprint_build(&input,NULL,&changed)==
          VPS_QUERY_METADATA_OK);
    CHECK(vps_query_fingerprint_compare(&base,&changed)==
          VPS_QUERY_CHANGE_POLICY);
    metadata.materialization=VPS_QUERY_MATERIALIZATION_OFF;
    metadata.key_columns[0]=0U; metadata.key_column_count=1U;
    CHECK(vps_query_fingerprint_build(&input,NULL,&changed)==
          VPS_QUERY_METADATA_OK);
    CHECK(vps_query_fingerprint_compare(&base,&changed)==
          VPS_QUERY_CHANGE_KEY);
    metadata.key_column_count=0U; metadata.indexes.index_count=1U;
    metadata.indexes.indexes[0].name_hash=77U;
    metadata.indexes.indexes[0].columns[0]=0U;
    metadata.indexes.indexes[0].column_count=1U;
    CHECK(vps_query_fingerprint_build(&input,NULL,&changed)==
          VPS_QUERY_METADATA_OK);
    CHECK(vps_query_fingerprint_compare(&base,&changed)==
          VPS_QUERY_CHANGE_INDEXES);
    metadata.indexes.index_count=0U; column.origin_relation_oid=900U;
    CHECK(vps_query_fingerprint_build(&input,NULL,&changed)==
          VPS_QUERY_METADATA_OK);
    CHECK(vps_query_fingerprint_compare(&base,&changed)==
          VPS_QUERY_CHANGE_LAYOUT);
    column.origin_relation_oid=0U; column.spatial_kind=VPS_QUERY_SPATIAL_GEOMETRY;
    column.spatial_srid=4326; column.spatial_dimensions=2U;
    CHECK(vps_query_fingerprint_build(&input,NULL,&changed)==
          VPS_QUERY_METADATA_OK);
    CHECK(vps_query_fingerprint_compare(&base,&changed)==
          VPS_QUERY_CHANGE_LAYOUT);
    column.spatial_kind=VPS_QUERY_SPATIAL_NONE; column.spatial_srid=0;
    column.spatial_dimensions=0U; input.wrapper_format_version+=1U;
    CHECK(vps_query_fingerprint_build(&input,NULL,&changed)==
          VPS_QUERY_METADATA_OK);
    CHECK(vps_query_fingerprint_compare(&base,&changed)==
          VPS_QUERY_CHANGE_WRAPPER);
    if(failures!=0)return 1;
    (void)puts("vps_query_fingerprint_test: passed"); return 0;
}
