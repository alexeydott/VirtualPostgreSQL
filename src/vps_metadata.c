#include "vps_metadata.h"

#include <string.h>

static const char vps_relation_sql[] =
    "SELECT n.oid::pg_catalog.text,c.oid::pg_catalog.text,c.relkind::pg_catalog.text,"
    "c.relpersistence::pg_catalog.text,c.relispartition::pg_catalog.text,"
    "c.relrowsecurity::pg_catalog.text,c.relforcerowsecurity::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_inherits i WHERE i.inhparent=c.oid))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_inherits i WHERE i.inhrelid=c.oid))::pg_catalog.text,"
    "n.nspname::pg_catalog.text,c.relname::pg_catalog.text,c.relam::pg_catalog.text "
    "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
    "WHERE n.nspname=$1::pg_catalog.name AND c.relname=$2::pg_catalog.name";

static const char vps_columns_sql[] =
    "SELECT a.attnum::pg_catalog.text,a.attname::pg_catalog.text,a.atttypid::pg_catalog.text,"
    "a.atttypmod::pg_catalog.text,t.typnamespace::pg_catalog.text,tn.nspname::pg_catalog.text,"
    "t.typname::pg_catalog.text,t.typcategory::pg_catalog.text,t.typtype::pg_catalog.text,"
    "t.typbasetype::pg_catalog.text,t.typtypmod::pg_catalog.text,t.typelem::pg_catalog.text,"
    "a.attnotnull::pg_catalog.text,a.attisdropped::pg_catalog.text,"
    "(d.adbin IS NOT NULL)::pg_catalog.text,a.attgenerated::pg_catalog.text,"
    "a.attidentity::pg_catalog.text,a.attcollation::pg_catalog.text,"
    "co.collname::pg_catalog.text,co.collprovider::pg_catalog.text,"
    "co.collisdeterministic::pg_catalog.text,a.attstorage::pg_catalog.text,"
    "a.attcompression::pg_catalog.text,COALESCE(a.attstattarget,-1)::pg_catalog.text,"
    "(des.description IS NOT NULL)::pg_catalog.text,a.attrelid::pg_catalog.text,"
    "a.attnum::pg_catalog.text,t.typnotnull::pg_catalog.text,"
    "bt.typnamespace::pg_catalog.text,btn.nspname::pg_catalog.text,"
    "bt.typname::pg_catalog.text,bt.typcategory::pg_catalog.text,"
    "bt.typtype::pg_catalog.text,"
    "CASE WHEN d.adbin IS NULL THEN NULL ELSE pg_catalog.md5("
    "pg_catalog.pg_get_expr(d.adbin,d.adrelid,true)) END::pg_catalog.text,"
    "CASE WHEN d.adbin IS NULL THEN '' WHEN a.attgenerated<>'' THEN 'g' "
    "WHEN pg_catalog.pg_get_expr(d.adbin,d.adrelid,true) LIKE 'nextval(%' "
    "THEN 's' ELSE 'u' END::pg_catalog.text,"
    "(t.typdefaultbin IS NOT NULL OR t.typdefault IS NOT NULL)::pg_catalog.text,"
    "CASE WHEN t.typdefaultbin IS NULL AND t.typdefault IS NULL THEN NULL "
    "ELSE pg_catalog.md5(COALESCE(t.typdefaultbin::pg_catalog.text,"
    "t.typdefault::pg_catalog.text)) END::pg_catalog.text,"
    "(SELECT pg_catalog.md5(pg_catalog.string_agg(pg_catalog.md5("
    "pg_catalog.concat_ws(':',dc.contype::pg_catalog.text,"
    "dc.convalidated::pg_catalog.text,dc.connoinherit::pg_catalog.text,"
    "dc.conbin::pg_catalog.text)),',' ORDER BY dc.oid)) "
    "FROM pg_catalog.pg_constraint dc WHERE dc.contypid=t.oid)::pg_catalog.text "
    ",pg_catalog.format_type(a.atttypid,a.atttypmod)::pg_catalog.text "
    "FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_type t ON t.oid=a.atttypid "
    "JOIN pg_catalog.pg_namespace tn ON tn.oid=t.typnamespace "
    "LEFT JOIN pg_catalog.pg_type bt ON bt.oid=NULLIF(t.typbasetype,0) "
    "LEFT JOIN pg_catalog.pg_namespace btn ON btn.oid=bt.typnamespace "
    "LEFT JOIN pg_catalog.pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum "
    "LEFT JOIN pg_catalog.pg_collation co ON co.oid=a.attcollation "
    "LEFT JOIN pg_catalog.pg_description des ON des.objoid=a.attrelid AND des.objsubid=a.attnum "
    "WHERE a.attrelid=$1::pg_catalog.oid AND a.attnum>0 ORDER BY a.attnum";

static const char vps_keys_sql[] =
    "SELECT i.indexrelid::pg_catalog.text,i.indisprimary::pg_catalog.text,"
    "i.indisunique::pg_catalog.text,i.indisvalid::pg_catalog.text,"
    "i.indisready::pg_catalog.text,i.indimmediate::pg_catalog.text,"
    "(i.indpred IS NOT NULL)::pg_catalog.text,(i.indexprs IS NOT NULL)::pg_catalog.text,"
    "i.indnullsnotdistinct::pg_catalog.text,i.indnkeyatts::pg_catalog.text,"
    "i.indnatts::pg_catalog.text,i.indkey::pg_catalog.text,"
    "co.condeferrable::pg_catalog.text,co.contype::pg_catalog.text,"
    "am.amname::pg_catalog.text,ic.relname::pg_catalog.text "
    "FROM pg_catalog.pg_index i JOIN pg_catalog.pg_class ic ON ic.oid=i.indexrelid "
    "JOIN pg_catalog.pg_am am ON am.oid=ic.relam "
    "LEFT JOIN pg_catalog.pg_constraint co ON co.conindid=i.indexrelid "
    "WHERE i.indrelid=$1::pg_catalog.oid ORDER BY i.indisprimary DESC,i.indexrelid";

static const char vps_policy_sql[] =
    "SELECT c.oid::pg_catalog.text,c.relkind::pg_catalog.text,"
    "c.relispartition::pg_catalog.text,c.relrowsecurity::pg_catalog.text,"
    "c.relforcerowsecurity::pg_catalog.text,i.inhparent::pg_catalog.text,"
    "p.partstrat::pg_catalog.text,p.partnatts::pg_catalog.text,"
    "p.partattrs::pg_catalog.text,(i.inhrelid IS NOT NULL)::pg_catalog.text "
    "FROM pg_catalog.pg_class c LEFT JOIN pg_catalog.pg_inherits i ON i.inhrelid=c.oid "
    "LEFT JOIN pg_catalog.pg_partitioned_table p ON p.partrelid=c.oid "
    "WHERE c.oid=$1::pg_catalog.oid ORDER BY i.inhseqno";

static const char vps_postgis_sql[] =
    "SELECT e.extversion::pg_catalog.text,e.extnamespace::pg_catalog.text,"
    "n.nspname::pg_catalog.text,COALESCE(g.oid,0)::pg_catalog.text,"
    "COALESCE(gg.oid,0)::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_astext'))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_geomfromtext'))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_asbinary'))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_geomfromwkb'))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_asewkt'))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_geomfromewkt'))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_asewkb'))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_geomfromewkb'))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_geogfromtext'))::pg_catalog.text,"
    "(EXISTS(SELECT 1 FROM pg_catalog.pg_proc p WHERE p.pronamespace=e.extnamespace AND p.proname='st_geogfromwkb'))::pg_catalog.text "
    "FROM pg_catalog.pg_extension e JOIN pg_catalog.pg_namespace n ON n.oid=e.extnamespace "
    "LEFT JOIN pg_catalog.pg_type g ON g.typnamespace=e.extnamespace AND g.typname='geometry' "
    "AND EXISTS(SELECT 1 FROM pg_catalog.pg_depend d WHERE d.classid='pg_catalog.pg_type'::pg_catalog.regclass AND d.objid=g.oid AND d.refclassid='pg_catalog.pg_extension'::pg_catalog.regclass AND d.refobjid=e.oid AND d.deptype='e') "
    "LEFT JOIN pg_catalog.pg_type gg ON gg.typnamespace=e.extnamespace AND gg.typname='geography' "
    "AND EXISTS(SELECT 1 FROM pg_catalog.pg_depend d WHERE d.classid='pg_catalog.pg_type'::pg_catalog.regclass AND d.objid=gg.oid AND d.refclassid='pg_catalog.pg_extension'::pg_catalog.regclass AND d.refobjid=e.oid AND d.deptype='e') "
    "WHERE e.extname=$1::pg_catalog.name";

/* Public metadata functions deliberately return only text-formatted catalog
 * scalars.  This keeps the shared async copy boundary independent of libpq
 * native widths and makes every value subject to the same byte limits. */
static const char vps_relations_function_sql[] =
    "SELECT n.nspname::pg_catalog.text,c.relname::pg_catalog.text,"
    "c.oid::pg_catalog.text,c.relkind::pg_catalog.text,"
    "c.relpersistence::pg_catalog.text,r.rolname::pg_catalog.text,"
    "ts.spcname::pg_catalog.text,c.reltuples::pg_catalog.text,"
    "c.relpages::pg_catalog.text,pg_catalog.pg_relation_size(c.oid)::pg_catalog.text,"
    "pg_catalog.pg_total_relation_size(c.oid)::pg_catalog.text,"
    "d.description::pg_catalog.text,c.relrowsecurity::pg_catalog.text,"
    "c.relforcerowsecurity::pg_catalog.text,c.relispartition::pg_catalog.text,"
    "pn.nspname::pg_catalog.text,pc.relname::pg_catalog.text,"
    "pc.oid::pg_catalog.text,"
    "(c.relkind IN ('r','p','v','m','f'))::pg_catalog.text,"
    "(c.relkind IN ('r','p'))::pg_catalog.text,"
    "(c.relkind IN ('r','p','v','m','f'))::pg_catalog.text,"
    "(c.reltuples>=0)::pg_catalog.text,"
    "CASE c.relkind WHEN 'r' THEN 'table' WHEN 'p' THEN 'partitioned_table' "
    "WHEN 'v' THEN 'view' WHEN 'm' THEN 'materialized_view' "
    "WHEN 'f' THEN 'foreign_table' ELSE 'other' END::pg_catalog.text "
    "FROM pg_catalog.pg_class c "
    "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
    "JOIN pg_catalog.pg_roles r ON r.oid=c.relowner "
    "LEFT JOIN pg_catalog.pg_tablespace ts ON ts.oid=c.reltablespace "
    "LEFT JOIN pg_catalog.pg_description d ON d.objoid=c.oid AND d.objsubid=0 "
    "LEFT JOIN pg_catalog.pg_inherits inh ON inh.inhrelid=c.oid AND inh.inhseqno=1 "
    "LEFT JOIN pg_catalog.pg_class pc ON pc.oid=inh.inhparent "
    "LEFT JOIN pg_catalog.pg_namespace pn ON pn.oid=pc.relnamespace "
    "WHERE n.nspname=$1::pg_catalog.name "
    "ORDER BY c.relname,c.oid LIMIT 65536";

static const char vps_table_info_function_sql[] =
    "SELECT (pg_catalog.row_number() OVER (ORDER BY a.attnum)-1)::pg_catalog.text,"
    "a.attname::pg_catalog.text,pg_catalog.format_type(a.atttypid,a.atttypmod)::pg_catalog.text,"
    "a.attnotnull::pg_catalog.text,pg_catalog.pg_get_expr(ad.adbin,ad.adrelid,true)::pg_catalog.text,"
    "(COALESCE(array_position(con.conkey,a.attnum),0))::pg_catalog.text,"
    "'0'::pg_catalog.text,a.attnum::pg_catalog.text,a.atttypid::pg_catalog.text,"
    "a.atttypmod::pg_catalog.text,tn.nspname::pg_catalog.text,t.typname::pg_catalog.text,"
    "dn.nspname::pg_catalog.text,dt.typname::pg_catalog.text,"
    "en.nspname::pg_catalog.text,et.typname::pg_catalog.text,"
    "co.collname::pg_catalog.text,a.attidentity::pg_catalog.text,"
    "a.attgenerated::pg_catalog.text,a.attstorage::pg_catalog.text,"
    "a.attcompression::pg_catalog.text,des.description::pg_catalog.text,"
    "CASE WHEN pe.extname='postgis' AND t.typname IN ('geometry','geography') "
    "THEN t.typname ELSE NULL END::pg_catalog.text,"
    "CASE WHEN pe.extname='postgis' AND a.atttypmod>=0 "
    "THEN ((a.atttypmod-4)>>8)::pg_catalog.text ELSE NULL END,"
    "CASE WHEN pe.extname='postgis' AND a.atttypmod>=0 "
    "THEN ((a.atttypmod-4)&255)::pg_catalog.text ELSE NULL END "
    "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
    "JOIN pg_catalog.pg_attribute a ON a.attrelid=c.oid "
    "JOIN pg_catalog.pg_type t ON t.oid=a.atttypid "
    "JOIN pg_catalog.pg_namespace tn ON tn.oid=t.typnamespace "
    "LEFT JOIN pg_catalog.pg_type dt ON dt.oid=NULLIF(t.typbasetype,0) "
    "LEFT JOIN pg_catalog.pg_namespace dn ON dn.oid=dt.typnamespace "
    "LEFT JOIN pg_catalog.pg_type et ON et.oid=NULLIF(t.typelem,0) "
    "LEFT JOIN pg_catalog.pg_namespace en ON en.oid=et.typnamespace "
    "LEFT JOIN pg_catalog.pg_collation co ON co.oid=a.attcollation "
    "LEFT JOIN pg_catalog.pg_attrdef ad ON ad.adrelid=a.attrelid AND ad.adnum=a.attnum "
    "LEFT JOIN pg_catalog.pg_constraint con ON con.conrelid=c.oid AND con.contype='p' "
    "LEFT JOIN pg_catalog.pg_description des ON des.objoid=c.oid AND des.objsubid=a.attnum "
    "LEFT JOIN pg_catalog.pg_depend pd ON pd.classid='pg_catalog.pg_type'::pg_catalog.regclass "
    "AND pd.objid=t.oid AND pd.refclassid='pg_catalog.pg_extension'::pg_catalog.regclass AND pd.deptype='e' "
    "LEFT JOIN pg_catalog.pg_extension pe ON pe.oid=pd.refobjid "
    "WHERE n.nspname=$1::pg_catalog.name AND c.relname=$2::pg_catalog.name "
    "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum LIMIT 65536";

static const char vps_index_list_function_sql[] =
    "SELECT (pg_catalog.row_number() OVER (ORDER BY i.indexrelid)-1)::pg_catalog.text,"
    "ic.relname::pg_catalog.text,i.indisunique::pg_catalog.text,"
    "CASE WHEN i.indisprimary THEN 'pk' WHEN con.oid IS NOT NULL THEN 'u' ELSE 'c' END::pg_catalog.text,"
    "(i.indpred IS NOT NULL)::pg_catalog.text,i.indisvalid::pg_catalog.text,"
    "i.indisready::pg_catalog.text,i.indimmediate::pg_catalog.text,"
    "i.indisprimary::pg_catalog.text,(con.contype='x')::pg_catalog.text,"
    "i.indnullsnotdistinct::pg_catalog.text,am.amname::pg_catalog.text,"
    "i.indnkeyatts::pg_catalog.text,(i.indnatts-i.indnkeyatts)::pg_catalog.text,"
    "pg_catalog.pg_get_expr(i.indpred,i.indrelid,true)::pg_catalog.text,"
    "(i.indexprs IS NOT NULL)::pg_catalog.text "
    "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
    "JOIN pg_catalog.pg_index i ON i.indrelid=c.oid "
    "JOIN pg_catalog.pg_class ic ON ic.oid=i.indexrelid "
    "JOIN pg_catalog.pg_am am ON am.oid=ic.relam "
    "LEFT JOIN pg_catalog.pg_constraint con ON con.conindid=i.indexrelid "
    "WHERE n.nspname=$1::pg_catalog.name AND c.relname=$2::pg_catalog.name "
    "ORDER BY i.indexrelid LIMIT 65536";

static const char vps_index_info_function_sql[] =
    "SELECT (ord.n-1)::pg_catalog.text,COALESCE(a.attnum,-1)::pg_catalog.text,"
    "a.attname::pg_catalog.text,"
    "((i.indoption[ord.n-1]&1)<>0)::pg_catalog.text,"
    "((i.indoption[ord.n-1]&2)<>0)::pg_catalog.text,"
    "co.collname::pg_catalog.text,opc.opcname::pg_catalog.text,"
    "(ord.n>i.indnkeyatts)::pg_catalog.text,"
    "CASE WHEN a.attnum IS NULL THEN pg_catalog.pg_get_indexdef(i.indexrelid,ord.n,false) ELSE NULL END::pg_catalog.text,"
    "(ord.n<=i.indnkeyatts)::pg_catalog.text,a.atttypid::pg_catalog.text,"
    "a.atttypmod::pg_catalog.text,ord.n::pg_catalog.text "
    "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
    "JOIN pg_catalog.pg_index i ON i.indrelid=c.oid "
    "JOIN pg_catalog.pg_class ic ON ic.oid=i.indexrelid "
    "CROSS JOIN LATERAL pg_catalog.generate_series(1,i.indnatts) ord(n) "
    "LEFT JOIN pg_catalog.pg_attribute a ON a.attrelid=c.oid AND a.attnum=i.indkey[ord.n-1] "
    "LEFT JOIN pg_catalog.pg_collation co ON co.oid=i.indcollation[ord.n-1] "
    "LEFT JOIN pg_catalog.pg_opclass opc ON opc.oid=i.indclass[ord.n-1] "
    "WHERE n.nspname=$1::pg_catalog.name AND c.relname=$2::pg_catalog.name "
    "AND ic.relname=$3::pg_catalog.name ORDER BY ord.n LIMIT 65536";

static const char vps_type_info_function_sql[] =
    "SELECT n.nspname::pg_catalog.text,t.typname::pg_catalog.text,t.oid::pg_catalog.text,"
    "t.typtype::pg_catalog.text,t.typcategory::pg_catalog.text,t.typlen::pg_catalog.text,"
    "t.typbyval::pg_catalog.text,t.typalign::pg_catalog.text,t.typstorage::pg_catalog.text,"
    "t.typnotnull::pg_catalog.text,t.typbasetype::pg_catalog.text,t.typtypmod::pg_catalog.text,"
    "t.typelem::pg_catalog.text,t.typarray::pg_catalog.text,c.collname::pg_catalog.text,"
    "e.extname::pg_catalog.text,e.extversion::pg_catalog.text,"
    "pg_catalog.format_type(t.oid,NULL)::pg_catalog.text "
    "FROM pg_catalog.pg_type t JOIN pg_catalog.pg_namespace n ON n.oid=t.typnamespace "
    "LEFT JOIN pg_catalog.pg_collation c ON c.oid=t.typcollation "
    "LEFT JOIN pg_catalog.pg_depend d ON d.classid='pg_catalog.pg_type'::pg_catalog.regclass "
    "AND d.objid=t.oid AND d.refclassid='pg_catalog.pg_extension'::pg_catalog.regclass AND d.deptype='e' "
    "LEFT JOIN pg_catalog.pg_extension e ON e.oid=d.refobjid "
    "WHERE n.nspname=$1::pg_catalog.name AND t.typname=$2::pg_catalog.name LIMIT 2";

static const char vps_extensions_function_sql[] =
    "SELECT e.extname::pg_catalog.text,e.extversion::pg_catalog.text,"
    "n.nspname::pg_catalog.text,e.extnamespace::pg_catalog.text,"
    "e.extrelocatable::pg_catalog.text,e.extconfig::pg_catalog.text,"
    "e.extcondition::pg_catalog.text,e.oid::pg_catalog.text "
    "FROM pg_catalog.pg_extension e JOIN pg_catalog.pg_namespace n ON n.oid=e.extnamespace "
    "ORDER BY e.extname LIMIT 65536";

static const VpsCatalogQuerySpec vps_catalog_specs[VPS_CATALOG_QUERY_COUNT] = {
    {VPS_CATALOG_QUERY_RELATION, vps_relation_sql,
     sizeof(vps_relation_sql) - 1U, 2U, 12U},
    {VPS_CATALOG_QUERY_COLUMNS, vps_columns_sql,
     sizeof(vps_columns_sql) - 1U, 1U, 39U},
    {VPS_CATALOG_QUERY_KEYS, vps_keys_sql,
     sizeof(vps_keys_sql) - 1U, 1U, 16U},
    {VPS_CATALOG_QUERY_RELATION_POLICY, vps_policy_sql,
     sizeof(vps_policy_sql) - 1U, 1U, 10U},
    {VPS_CATALOG_QUERY_POSTGIS, vps_postgis_sql,
     sizeof(vps_postgis_sql) - 1U, 1U, 15U},
    {VPS_CATALOG_QUERY_RELATIONS_FUNCTION, vps_relations_function_sql,
     sizeof(vps_relations_function_sql) - 1U, 1U, 23U},
    {VPS_CATALOG_QUERY_TABLE_INFO_FUNCTION, vps_table_info_function_sql,
     sizeof(vps_table_info_function_sql) - 1U, 2U, 25U},
    {VPS_CATALOG_QUERY_INDEX_LIST_FUNCTION, vps_index_list_function_sql,
     sizeof(vps_index_list_function_sql) - 1U, 2U, 16U},
    {VPS_CATALOG_QUERY_INDEX_INFO_FUNCTION, vps_index_info_function_sql,
     sizeof(vps_index_info_function_sql) - 1U, 3U, 13U},
    {VPS_CATALOG_QUERY_TYPE_INFO_FUNCTION, vps_type_info_function_sql,
     sizeof(vps_type_info_function_sql) - 1U, 2U, 18U},
    {VPS_CATALOG_QUERY_EXTENSIONS_FUNCTION, vps_extensions_function_sql,
     sizeof(vps_extensions_function_sql) - 1U, 0U, 8U}};

static void vps_metadata_log(VpsMetadataRowSet *rowset,
                             VpsCatalogQuery query,
                             const char *status,
                             size_t rows,
                             size_t fields,
                             size_t bytes)
{
    VpsLogEvent event;
    const char *operation = vps_catalog_query_name(query);
    static const char phase[] = "catalog_copy";
    if (rowset == NULL || rowset->logger == NULL || status == NULL ||
        vps_log_event_init(&event, strcmp(status, "passed") == 0
                                       ? VPS_LOG_LEVEL_DEBUG
                                       : VPS_LOG_LEVEL_WARN) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 strlen(operation)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 sizeof(phase) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_ROW_COUNT,
                                 (uint64_t)rows) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_RESULT_FIELD_COUNT,
                                 (uint64_t)fields) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_BYTE_COUNT,
                                 (uint64_t)bytes) != VPS_LOG_OK) return;
    vps_logger_emit(rowset->logger, &event);
}

VpsMetadataResult vps_metadata_catalog_query_spec(VpsCatalogQuery query,
                                                   VpsCatalogQuerySpec *spec)
{
    if (spec == NULL || query < VPS_CATALOG_QUERY_RELATION ||
        query >= VPS_CATALOG_QUERY_COUNT) return VPS_METADATA_INVALID_ARGUMENT;
    *spec = vps_catalog_specs[(size_t)query];
    return VPS_METADATA_OK;
}

VpsMetadataResult vps_metadata_rowset_init(VpsMetadataRowSet *rowset,
                                           const VpsAllocator *allocator,
                                           VpsLogger *logger)
{
    if (rowset == NULL || !vps_allocator_is_valid(allocator))
        return VPS_METADATA_INVALID_ARGUMENT;
    (void)memset(rowset, 0, sizeof(*rowset));
    rowset->allocator = *allocator;
    rowset->logger = logger;
    rowset->initialized = 1;
    return VPS_METADATA_OK;
}

VpsMetadataResult vps_metadata_rowset_copy(VpsMetadataRowSet *rowset,
                                           VpsCatalogQuery query,
                                           const VpsMetadataInput *input)
{
    VpsCatalogQuerySpec spec;
    VpsMetadataCell *cells = NULL;
    unsigned char *bytes = NULL;
    size_t cell_count;
    size_t cell_bytes;
    size_t total_bytes = 0U;
    size_t row;
    size_t field;
    size_t byte_offset = 0U;
    VpsMemoryResult memory_result;
    if (rowset == NULL || !rowset->initialized || input == NULL ||
        input->is_null == NULL || input->value == NULL ||
        input->length == NULL ||
        vps_metadata_catalog_query_spec(query, &spec) != VPS_METADATA_OK)
        return VPS_METADATA_INVALID_ARGUMENT;
    if (input->field_count != spec.result_field_count ||
        input->field_count > VPS_METADATA_MAX_FIELDS ||
        input->row_count > VPS_METADATA_MAX_ROWS) {
        vps_metadata_log(rowset, query, "invalid_result", input->row_count,
                         input->field_count, 0U);
        return VPS_METADATA_INVALID_RESULT;
    }
    memory_result = vps_size_multiply(input->row_count, input->field_count,
                                      &cell_count);
    if (memory_result != VPS_MEMORY_OK || cell_count > VPS_METADATA_MAX_CELLS)
        return VPS_METADATA_LIMIT_EXCEEDED;
    for (row = 0U; row < input->row_count; ++row) {
        for (field = 0U; field < input->field_count; ++field) {
            size_t length;
            if (input->is_null(input->context, row, field)) continue;
            length = input->length(input->context, row, field);
            if (length > VPS_METADATA_MAX_CELL_BYTES ||
                input->value(input->context, row, field) == NULL ||
                vps_size_add(total_bytes, length, &total_bytes) !=
                    VPS_MEMORY_OK ||
                total_bytes > VPS_METADATA_MAX_TOTAL_BYTES) {
                vps_metadata_log(rowset, query, "limit_exceeded",
                                 input->row_count, input->field_count,
                                 total_bytes);
                return VPS_METADATA_LIMIT_EXCEEDED;
            }
        }
    }
    memory_result = vps_size_multiply(cell_count, sizeof(*cells), &cell_bytes);
    if (memory_result != VPS_MEMORY_OK) return VPS_METADATA_LIMIT_EXCEEDED;
    if (cell_bytes != 0U &&
        vps_memory_allocate(&rowset->allocator, cell_bytes,
                            (void **)&cells) != VPS_MEMORY_OK)
        return VPS_METADATA_OUT_OF_MEMORY;
    if (total_bytes != 0U &&
        vps_memory_allocate(&rowset->allocator, total_bytes,
                            (void **)&bytes) != VPS_MEMORY_OK) {
        vps_memory_release(&rowset->allocator, (void **)&cells, cell_bytes);
        return VPS_METADATA_OUT_OF_MEMORY;
    }
    if ((cell_count != 0U && cells == NULL) ||
        (total_bytes != 0U && bytes == NULL)) {
        vps_memory_release(&rowset->allocator, (void **)&bytes, total_bytes);
        vps_memory_release(&rowset->allocator, (void **)&cells, cell_bytes);
        return VPS_METADATA_OUT_OF_MEMORY;
    }
    if (cell_bytes != 0U) (void)memset(cells, 0, cell_bytes);
    for (row = 0U; row < input->row_count; ++row) {
        for (field = 0U; field < input->field_count; ++field) {
            size_t index = row * input->field_count + field;
            if (index >= cell_count) {
                vps_memory_release(&rowset->allocator, (void **)&bytes,
                                   total_bytes);
                vps_memory_release(&rowset->allocator, (void **)&cells,
                                   cell_bytes);
                return VPS_METADATA_INVALID_RESULT;
            }
            cells[index].is_null = input->is_null(input->context, row, field);
            cells[index].offset = byte_offset;
            if (!cells[index].is_null) {
                const void *value = input->value(input->context, row, field);
                cells[index].length = input->length(input->context, row, field);
                if (cells[index].length != 0U) {
                    if (value == NULL || byte_offset > total_bytes ||
                        cells[index].length > total_bytes - byte_offset) {
                        vps_memory_release(&rowset->allocator, (void **)&bytes,
                                           total_bytes);
                        vps_memory_release(&rowset->allocator, (void **)&cells,
                                           cell_bytes);
                        return VPS_METADATA_INVALID_RESULT;
                    }
                    (void)memcpy(bytes + byte_offset, value,
                                 cells[index].length);
                    byte_offset += cells[index].length;
                }
            }
        }
    }
    vps_metadata_rowset_reset(rowset);
    rowset->cells = cells;
    rowset->bytes = bytes;
    rowset->row_count = input->row_count;
    rowset->field_count = input->field_count;
    rowset->cell_count = cell_count;
    rowset->cell_bytes = cell_bytes;
    rowset->bytes_size = total_bytes;
    rowset->initialized = 1;
    vps_metadata_log(rowset, query, "passed", rowset->row_count,
                     rowset->field_count, rowset->bytes_size);
    return VPS_METADATA_OK;
}

VpsMetadataResult vps_metadata_rowset_append(VpsMetadataRowSet *rowset,
                                             VpsCatalogQuery query,
                                             const VpsMetadataInput *input)
{
    VpsCatalogQuerySpec spec;
    VpsMetadataCell *cells = NULL;
    unsigned char *bytes = NULL;
    size_t new_rows;
    size_t cell_count;
    size_t cell_bytes;
    size_t added_bytes = 0U;
    size_t total_bytes;
    size_t row;
    size_t field;
    size_t offset;
    if (rowset == NULL || !rowset->initialized || input == NULL ||
        input->is_null == NULL || input->value == NULL ||
        input->length == NULL ||
        vps_metadata_catalog_query_spec(query, &spec) != VPS_METADATA_OK)
        return VPS_METADATA_INVALID_ARGUMENT;
    if (input->field_count != spec.result_field_count ||
        input->field_count > VPS_METADATA_MAX_FIELDS ||
        (rowset->row_count != 0U && rowset->field_count != input->field_count) ||
        vps_size_add(rowset->row_count, input->row_count, &new_rows) !=
            VPS_MEMORY_OK ||
        new_rows > VPS_METADATA_MAX_ROWS)
        return VPS_METADATA_INVALID_RESULT;
    for (row = 0U; row < input->row_count; ++row) {
        for (field = 0U; field < input->field_count; ++field) {
            size_t length;
            if (input->is_null(input->context, row, field)) continue;
            length = input->length(input->context, row, field);
            if (length > VPS_METADATA_MAX_CELL_BYTES ||
                input->value(input->context, row, field) == NULL ||
                vps_size_add(added_bytes, length, &added_bytes) !=
                    VPS_MEMORY_OK ||
                added_bytes > VPS_METADATA_MAX_TOTAL_BYTES)
                return VPS_METADATA_LIMIT_EXCEEDED;
        }
    }
    if (vps_size_add(rowset->bytes_size, added_bytes, &total_bytes) !=
            VPS_MEMORY_OK ||
        total_bytes > VPS_METADATA_MAX_TOTAL_BYTES ||
        vps_size_multiply(new_rows, input->field_count, &cell_count) !=
            VPS_MEMORY_OK ||
        cell_count > VPS_METADATA_MAX_CELLS ||
        vps_size_multiply(cell_count, sizeof(*cells), &cell_bytes) !=
            VPS_MEMORY_OK)
        return VPS_METADATA_LIMIT_EXCEEDED;
    if ((rowset->cell_bytes != 0U && rowset->cells == NULL) ||
        (rowset->bytes_size != 0U && rowset->bytes == NULL) ||
        rowset->cell_bytes > cell_bytes || rowset->bytes_size > total_bytes)
        return VPS_METADATA_INVALID_RESULT;
    if (cell_bytes != 0U &&
        vps_memory_allocate(&rowset->allocator, cell_bytes,
                            (void **)&cells) != VPS_MEMORY_OK)
        return VPS_METADATA_OUT_OF_MEMORY;
    if (total_bytes != 0U &&
        vps_memory_allocate(&rowset->allocator, total_bytes,
                            (void **)&bytes) != VPS_MEMORY_OK) {
        vps_memory_release(&rowset->allocator, (void **)&cells, cell_bytes);
        return VPS_METADATA_OUT_OF_MEMORY;
    }
    if ((cell_count != 0U && cells == NULL) ||
        (total_bytes != 0U && bytes == NULL)) {
        vps_memory_release(&rowset->allocator, (void **)&bytes, total_bytes);
        vps_memory_release(&rowset->allocator, (void **)&cells, cell_bytes);
        return VPS_METADATA_OUT_OF_MEMORY;
    }
    if (cell_bytes != 0U) (void)memset(cells, 0, cell_bytes);
    if (rowset->cell_bytes != 0U)
        (void)memcpy(cells, rowset->cells, rowset->cell_bytes);
    if (rowset->bytes_size != 0U)
        (void)memcpy(bytes, rowset->bytes, rowset->bytes_size);
    offset = rowset->bytes_size;
    for (row = 0U; row < input->row_count; ++row) {
        for (field = 0U; field < input->field_count; ++field) {
            size_t index = (rowset->row_count + row) * input->field_count + field;
            if (index >= cell_count) {
                vps_memory_release(&rowset->allocator, (void **)&bytes,
                                   total_bytes);
                vps_memory_release(&rowset->allocator, (void **)&cells,
                                   cell_bytes);
                return VPS_METADATA_INVALID_RESULT;
            }
            cells[index].is_null = input->is_null(input->context, row, field);
            cells[index].offset = offset;
            if (!cells[index].is_null) {
                const void *value = input->value(input->context, row, field);
                cells[index].length = input->length(input->context, row, field);
                if (cells[index].length != 0U) {
                    if (value == NULL || offset > total_bytes ||
                        cells[index].length > total_bytes - offset) {
                        vps_memory_release(&rowset->allocator, (void **)&bytes,
                                           total_bytes);
                        vps_memory_release(&rowset->allocator, (void **)&cells,
                                           cell_bytes);
                        return VPS_METADATA_INVALID_RESULT;
                    }
                    (void)memcpy(bytes + offset, value, cells[index].length);
                    offset += cells[index].length;
                }
            }
        }
    }
    vps_memory_release(&rowset->allocator, (void **)&rowset->bytes,
                       rowset->bytes_size);
    vps_memory_release(&rowset->allocator, (void **)&rowset->cells,
                       rowset->cell_bytes);
    rowset->cells = cells;
    rowset->bytes = bytes;
    rowset->row_count = new_rows;
    rowset->field_count = input->field_count;
    rowset->cell_count = cell_count;
    rowset->cell_bytes = cell_bytes;
    rowset->bytes_size = total_bytes;
    vps_metadata_log(rowset, query, "passed", rowset->row_count,
                     rowset->field_count, rowset->bytes_size);
    return VPS_METADATA_OK;
}

VpsMetadataResult vps_metadata_rowset_cell(
    const VpsMetadataRowSet *rowset,
    size_t row,
    size_t field,
    const unsigned char **value,
    size_t *length,
    int *is_null)
{
    const VpsMetadataCell *cell;
    if (rowset == NULL || !rowset->initialized || value == NULL ||
        length == NULL || is_null == NULL || row >= rowset->row_count ||
        field >= rowset->field_count) return VPS_METADATA_INVALID_ARGUMENT;
    cell = &rowset->cells[row * rowset->field_count + field];
    if (cell->offset > rowset->bytes_size ||
        cell->length > rowset->bytes_size - cell->offset)
        return VPS_METADATA_INVALID_STATE;
    *is_null = cell->is_null;
    *length = cell->length;
    *value = cell->is_null ? NULL : rowset->bytes + cell->offset;
    return VPS_METADATA_OK;
}

void vps_metadata_rowset_reset(VpsMetadataRowSet *rowset)
{
    VpsAllocator allocator;
    VpsLogger *logger;
    int initialized;
    if (rowset == NULL) return;
    allocator = rowset->allocator;
    logger = rowset->logger;
    initialized = rowset->initialized;
    if (initialized && vps_allocator_is_valid(&allocator)) {
        vps_memory_release(&allocator, (void **)&rowset->bytes,
                           rowset->bytes_size);
        vps_memory_release(&allocator, (void **)&rowset->cells,
                           rowset->cell_bytes);
    }
    (void)memset(rowset, 0, sizeof(*rowset));
    if (initialized) {
        rowset->allocator = allocator;
        rowset->logger = logger;
        rowset->initialized = 1;
    }
}

static int vps_metadata_parse_uint32(const unsigned char *value,
                                     size_t length,
                                     uint32_t *parsed)
{
    uint32_t result = 0U;
    size_t index;
    if (value == NULL || parsed == NULL || length == 0U || length > 10U)
        return 0;
    for (index = 0U; index < length; ++index) {
        uint32_t digit;
        if (value[index] < (unsigned char)'0' ||
            value[index] > (unsigned char)'9') return 0;
        digit = (uint32_t)(value[index] - (unsigned char)'0');
        if (result > (UINT32_MAX - digit) / 10U) return 0;
        result = result * 10U + digit;
    }
    *parsed = result;
    return 1;
}

static int vps_metadata_parse_int32(const unsigned char *value,
                                    size_t length,
                                    int32_t *parsed)
{
    uint32_t magnitude = 0U;
    size_t index = 0U;
    int negative = 0;
    if (value == NULL || parsed == NULL || length == 0U || length > 11U)
        return 0;
    if (value[0] == (unsigned char)'-') {
        negative = 1;
        index = 1U;
        if (length == 1U) return 0;
    }
    for (; index < length; ++index) {
        uint32_t digit;
        uint32_t limit = negative ? UINT32_C(2147483648)
                                  : UINT32_C(2147483647);
        if (value[index] < (unsigned char)'0' ||
            value[index] > (unsigned char)'9') return 0;
        digit = (uint32_t)(value[index] - (unsigned char)'0');
        if (magnitude > (limit - digit) / 10U) return 0;
        magnitude = magnitude * 10U + digit;
    }
    if (negative) {
        if (magnitude == UINT32_C(2147483648)) *parsed = INT32_MIN;
        else *parsed = -(int32_t)magnitude;
    } else {
        *parsed = (int32_t)magnitude;
    }
    return 1;
}

static int vps_metadata_parse_bool(const unsigned char *value,
                                   size_t length,
                                   int *parsed)
{
    if (value == NULL || parsed == NULL) return 0;
    if ((length == 1U && value[0] == (unsigned char)'t') ||
        (length == 4U && memcmp(value, "true", 4U) == 0)) {
        *parsed = 1;
        return 1;
    }
    if ((length == 1U && value[0] == (unsigned char)'f') ||
        (length == 5U && memcmp(value, "false", 5U) == 0)) {
        *parsed = 0;
        return 1;
    }
    return 0;
}

static int vps_metadata_cell_required(const VpsMetadataRowSet *rowset,
                                      size_t row,
                                      size_t field,
                                      const unsigned char **value,
                                      size_t *length)
{
    int is_null;
    return vps_metadata_rowset_cell(rowset, row, field, value, length,
                                    &is_null) == VPS_METADATA_OK &&
           !is_null && *value != NULL;
}

static VpsRelationKind vps_relation_kind_from_catalog(char relkind,
                                                       int is_partition,
                                                       int has_children)
{
    if (is_partition) return VPS_RELATION_PARTITION;
    switch (relkind) {
        case 'r':
            return has_children ? VPS_RELATION_INHERITANCE_PARENT
                                : VPS_RELATION_TABLE;
        case 'p': return VPS_RELATION_PARTITIONED_TABLE;
        case 'v': return VPS_RELATION_VIEW;
        case 'm': return VPS_RELATION_MATERIALIZED_VIEW;
        case 'f': return VPS_RELATION_FOREIGN_TABLE;
        case 'S': return VPS_RELATION_SEQUENCE;
        case 'c': return VPS_RELATION_COMPOSITE;
        case 't': return VPS_RELATION_TOAST;
        case 'i':
        case 'I': return VPS_RELATION_INDEX;
        default: return VPS_RELATION_UNSUPPORTED;
    }
}

VpsMetadataResult vps_relation_metadata_init(
    VpsRelationMetadata *relation,
    const VpsAllocator *allocator,
    VpsLogger *logger)
{
    if (relation == NULL || !vps_allocator_is_valid(allocator))
        return VPS_METADATA_INVALID_ARGUMENT;
    (void)memset(relation, 0, sizeof(*relation));
    relation->allocator = *allocator;
    relation->logger = logger;
    if (vps_buffer_init(&relation->schema_name, allocator,
                        VPS_METADATA_NAME_MAX_BYTES) != VPS_MEMORY_OK ||
        vps_buffer_init(&relation->relation_name, allocator,
                        VPS_METADATA_NAME_MAX_BYTES) != VPS_MEMORY_OK) {
        vps_buffer_reset(&relation->schema_name);
        vps_buffer_reset(&relation->relation_name);
        return VPS_METADATA_OUT_OF_MEMORY;
    }
    relation->initialized = 1;
    return VPS_METADATA_OK;
}

VpsMetadataResult vps_relation_metadata_resolve(
    VpsRelationMetadata *relation,
    const VpsMetadataRowSet *rowset,
    const char *expected_schema,
    size_t expected_schema_length,
    const char *expected_relation,
    size_t expected_relation_length)
{
    const unsigned char *values[12];
    size_t lengths[12];
    VpsRelationMetadata candidate;
    size_t field;
    uint32_t ignored;
    if (relation == NULL || !relation->initialized || rowset == NULL ||
        !rowset->initialized || expected_schema == NULL ||
        expected_relation == NULL || expected_schema_length == 0U ||
        expected_relation_length == 0U ||
        expected_schema_length > VPS_METADATA_NAME_MAX_BYTES ||
        expected_relation_length > VPS_METADATA_NAME_MAX_BYTES)
        return VPS_METADATA_INVALID_ARGUMENT;
    if (rowset->field_count != 12U) return VPS_METADATA_INVALID_RESULT;
    if (rowset->row_count == 0U) return VPS_METADATA_NOT_FOUND;
    if (rowset->row_count != 1U) return VPS_METADATA_INVALID_RESULT;
    for (field = 0U; field < 12U; ++field) {
        if (!vps_metadata_cell_required(rowset, 0U, field, &values[field],
                                        &lengths[field]))
            return VPS_METADATA_INVALID_RESULT;
    }
    if (lengths[9] != expected_schema_length ||
        memcmp(values[9], expected_schema, expected_schema_length) != 0 ||
        lengths[10] != expected_relation_length ||
        memcmp(values[10], expected_relation, expected_relation_length) != 0)
        return VPS_METADATA_INVALID_RESULT;
    if (vps_relation_metadata_init(&candidate, &relation->allocator,
                                   relation->logger) != VPS_METADATA_OK)
        return VPS_METADATA_OUT_OF_MEMORY;
    if (!vps_metadata_parse_uint32(values[0], lengths[0],
                                   &candidate.namespace_oid) ||
        !vps_metadata_parse_uint32(values[1], lengths[1],
                                   &candidate.relation_oid) ||
        lengths[2] != 1U || lengths[3] != 1U ||
        !vps_metadata_parse_bool(values[4], lengths[4],
                                 &candidate.is_partition) ||
        !vps_metadata_parse_bool(values[5], lengths[5],
                                 &candidate.row_security) ||
        !vps_metadata_parse_bool(values[6], lengths[6],
                                 &candidate.force_row_security) ||
        !vps_metadata_parse_bool(values[7], lengths[7],
                                 &candidate.has_children) ||
        !vps_metadata_parse_bool(values[8], lengths[8],
                                 &candidate.has_parent) ||
        !vps_metadata_parse_uint32(values[11], lengths[11], &ignored) ||
        vps_buffer_append(&candidate.schema_name, values[9], lengths[9]) !=
            VPS_MEMORY_OK ||
        vps_buffer_append(&candidate.relation_name, values[10], lengths[10]) !=
            VPS_MEMORY_OK) {
        vps_relation_metadata_reset(&candidate);
        return VPS_METADATA_INVALID_RESULT;
    }
    candidate.access_method_oid = ignored;
    candidate.persistence = (char)values[3][0];
    candidate.kind = vps_relation_kind_from_catalog(
        (char)values[2][0], candidate.is_partition, candidate.has_children);
    switch (candidate.kind) {
        case VPS_RELATION_TABLE:
        case VPS_RELATION_PARTITIONED_TABLE:
        case VPS_RELATION_PARTITION:
            candidate.readable = 1;
            candidate.writable_candidate = 1;
            break;
        case VPS_RELATION_VIEW:
        case VPS_RELATION_MATERIALIZED_VIEW:
        case VPS_RELATION_FOREIGN_TABLE:
        case VPS_RELATION_INHERITANCE_PARENT:
            candidate.readable = 1;
            candidate.writable_candidate = 0;
            break;
        default:
            vps_relation_metadata_reset(&candidate);
            return VPS_METADATA_UNSUPPORTED;
    }
    vps_relation_metadata_reset(relation);
    *relation = candidate;
    return VPS_METADATA_OK;
}

void vps_relation_metadata_reset(VpsRelationMetadata *relation)
{
    VpsAllocator allocator;
    VpsLogger *logger;
    int initialized;
    if (relation == NULL) return;
    allocator = relation->allocator;
    logger = relation->logger;
    initialized = relation->initialized;
    vps_buffer_reset(&relation->relation_name);
    vps_buffer_reset(&relation->schema_name);
    (void)memset(relation, 0, sizeof(*relation));
    if (initialized && vps_allocator_is_valid(&allocator)) {
        relation->allocator = allocator;
        relation->logger = logger;
        (void)vps_buffer_init(&relation->schema_name, &allocator,
                              VPS_METADATA_NAME_MAX_BYTES);
        (void)vps_buffer_init(&relation->relation_name, &allocator,
                              VPS_METADATA_NAME_MAX_BYTES);
        relation->initialized = 1;
    }
}

static int vps_metadata_optional_cell(const VpsMetadataRowSet *rowset,
                                      size_t row,
                                      size_t field,
                                      const unsigned char **value,
                                      size_t *length,
                                      int *present)
{
    int is_null;
    if (vps_metadata_rowset_cell(rowset, row, field, value, length,
                                 &is_null) != VPS_METADATA_OK) return 0;
    *present = !is_null;
    return is_null || *value != NULL;
}

static int vps_metadata_char(const unsigned char *value,
                             size_t length,
                             char *parsed,
                             int allow_empty)
{
    if (parsed == NULL || value == NULL ||
        (length != 1U && !(allow_empty && length == 0U))) return 0;
    *parsed = length == 0U ? '\0' : (char)value[0];
    return 1;
}

static VpsMetadataResult vps_column_string_append(
    VpsColumnSet *columns,
    VpsMetadataString *target,
    const unsigned char *value,
    size_t length,
    int present)
{
    VpsMemoryResult result;
    target->offset = columns->text.size;
    target->length = length;
    target->present = present;
    if (!present) return VPS_METADATA_OK;
    if (length > VPS_METADATA_MAX_CELL_BYTES)
        return VPS_METADATA_LIMIT_EXCEEDED;
    result = vps_buffer_append(&columns->text, value, length);
    if (result == VPS_MEMORY_OK) return VPS_METADATA_OK;
    return result == VPS_MEMORY_OUT_OF_MEMORY ? VPS_METADATA_OUT_OF_MEMORY
                                               : VPS_METADATA_LIMIT_EXCEEDED;
}

VpsMetadataResult vps_column_set_init(VpsColumnSet *columns,
                                      const VpsAllocator *allocator,
                                      VpsLogger *logger)
{
    if (columns == NULL || !vps_allocator_is_valid(allocator))
        return VPS_METADATA_INVALID_ARGUMENT;
    (void)memset(columns, 0, sizeof(*columns));
    columns->allocator = *allocator;
    columns->logger = logger;
    if (vps_buffer_init(&columns->text, allocator,
                        VPS_METADATA_MAX_TOTAL_BYTES) != VPS_MEMORY_OK)
        return VPS_METADATA_OUT_OF_MEMORY;
    columns->initialized = 1;
    return VPS_METADATA_OK;
}

VpsMetadataResult vps_column_set_build(VpsColumnSet *columns,
                                       const VpsMetadataRowSet *rowset)
{
    VpsColumnSet candidate;
    size_t columns_bytes;
    size_t row;
    if (columns == NULL || !columns->initialized || rowset == NULL ||
        !rowset->initialized || rowset->field_count != 39U)
        return VPS_METADATA_INVALID_ARGUMENT;
    if (vps_size_multiply(rowset->row_count, sizeof(VpsColumnMetadata),
                          &columns_bytes) != VPS_MEMORY_OK)
        return VPS_METADATA_LIMIT_EXCEEDED;
    if (vps_column_set_init(&candidate, &columns->allocator,
                            columns->logger) != VPS_METADATA_OK)
        return VPS_METADATA_OUT_OF_MEMORY;
    if (columns_bytes != 0U &&
        vps_memory_allocate(&candidate.allocator, columns_bytes,
                            (void **)&candidate.columns) != VPS_MEMORY_OK) {
        vps_column_set_reset(&candidate);
        return VPS_METADATA_OUT_OF_MEMORY;
    }
    candidate.columns_bytes = columns_bytes;
    candidate.column_count = rowset->row_count;
    if (columns_bytes != 0U)
        (void)memset(candidate.columns, 0, columns_bytes);
    for (row = 0U; row < rowset->row_count; ++row) {
        VpsColumnMetadata *column = &candidate.columns[row];
        const unsigned char *value[39];
        size_t length[39];
        int present[39];
        size_t field;
        VpsMetadataResult append_result;
        for (field = 0U; field < 39U; ++field) {
            if (!vps_metadata_optional_cell(rowset, row, field,
                                            &value[field], &length[field],
                                            &present[field])) goto invalid;
        }
        column->collation_deterministic = 1;
        if (!present[0] ||
            !vps_metadata_parse_int32(value[0], length[0],
                                      &column->attribute_number) ||
            !present[2] ||
            !vps_metadata_parse_uint32(value[2], length[2],
                                       &column->type_oid) ||
            !present[3] ||
            !vps_metadata_parse_int32(value[3], length[3],
                                      &column->type_modifier) ||
            !present[4] ||
            !vps_metadata_parse_uint32(value[4], length[4],
                                       &column->type_namespace_oid) ||
            !present[1] || !present[5] || !present[6] || !present[7] ||
            !present[8] ||
            !vps_metadata_char(value[7], length[7], &column->type_category, 0) ||
            !vps_metadata_char(value[8], length[8], &column->type_kind, 0) ||
            !present[9] ||
            !vps_metadata_parse_uint32(value[9], length[9],
                                       &column->domain_base_oid) ||
            !present[10] ||
            !vps_metadata_parse_int32(value[10], length[10],
                                      &column->domain_base_modifier) ||
            !present[11] ||
            !vps_metadata_parse_uint32(value[11], length[11],
                                       &column->array_element_oid) ||
            !present[12] ||
            !vps_metadata_parse_bool(value[12], length[12],
                                     &column->not_null) ||
            !present[13] ||
            !vps_metadata_parse_bool(value[13], length[13],
                                     &column->dropped) ||
            !present[14] ||
            !vps_metadata_parse_bool(value[14], length[14],
                                     &column->has_default) ||
            !present[15] ||
            !vps_metadata_char(value[15], length[15],
                               &column->generated_kind, 1) ||
            !present[16] ||
            !vps_metadata_char(value[16], length[16],
                               &column->identity_kind, 1) ||
            !present[17] ||
            !vps_metadata_parse_uint32(value[17], length[17],
                                       &column->collation_oid) ||
            (present[19] &&
             !vps_metadata_char(value[19], length[19],
                                &column->collation_provider, 0)) ||
            (present[20] &&
             !vps_metadata_parse_bool(value[20], length[20],
                                      &column->collation_deterministic)) ||
            !present[21] ||
            !vps_metadata_char(value[21], length[21],
                               &column->storage_kind, 0) ||
            !present[22] ||
            !vps_metadata_char(value[22], length[22],
                               &column->compression_kind, 1) ||
            !present[23] ||
            !vps_metadata_parse_int32(value[23], length[23],
                                      &column->statistics_target) ||
            !present[24] ||
            !vps_metadata_parse_bool(value[24], length[24],
                                     &column->has_comment) ||
            !present[25] ||
            !vps_metadata_parse_uint32(value[25], length[25],
                                       &column->origin_relation_oid) ||
            !present[26] ||
            !vps_metadata_parse_int32(value[26], length[26],
                                      &column->origin_attribute_number) ||
            !present[27] ||
            !vps_metadata_parse_bool(value[27], length[27],
                                     &column->domain_not_null) ||
            (present[31] &&
             !vps_metadata_char(value[31], length[31],
                                &column->domain_base_category, 0)) ||
            (present[32] &&
             !vps_metadata_char(value[32], length[32],
                                &column->domain_base_kind, 0)) ||
            !present[34] ||
            !vps_metadata_char(value[34], length[34],
                               &column->default_kind, 1) ||
            !present[35] ||
            !vps_metadata_parse_bool(value[35], length[35],
                                     &column->domain_has_default)) goto invalid;
        append_result = vps_column_string_append(
            &candidate, &column->name, value[1], length[1], present[1]);
        if (append_result == VPS_METADATA_OK)
            append_result = vps_column_string_append(
                &candidate, &column->type_namespace, value[5], length[5],
                present[5]);
        if (append_result == VPS_METADATA_OK)
            append_result = vps_column_string_append(
                &candidate, &column->type_name, value[6], length[6],
                present[6]);
        if (append_result == VPS_METADATA_OK)
            append_result = vps_column_string_append(
                &candidate, &column->domain_base_namespace, value[29],
                length[29], present[29]);
        if (append_result == VPS_METADATA_OK)
            append_result = vps_column_string_append(
                &candidate, &column->domain_base_name, value[30], length[30],
                present[30]);
        if (append_result == VPS_METADATA_OK)
            append_result = vps_column_string_append(
                &candidate, &column->collation_name, value[18], length[18],
                present[18]);
        if (append_result == VPS_METADATA_OK)
            append_result = vps_column_string_append(
                &candidate, &column->default_expression_hash, value[33],
                length[33], present[33]);
        if (append_result == VPS_METADATA_OK)
            append_result = vps_column_string_append(
                &candidate, &column->domain_default_hash, value[36],
                length[36], present[36]);
        if (append_result == VPS_METADATA_OK)
            append_result = vps_column_string_append(
                &candidate, &column->domain_constraint_hash, value[37],
                length[37], present[37]);
        if (append_result == VPS_METADATA_OK)
            append_result = vps_column_string_append(
                &candidate, &column->formatted_type, value[38],
                length[38], present[38]);
        if (append_result != VPS_METADATA_OK) {
            vps_column_set_reset(&candidate);
            return append_result;
        }
        if (!column->dropped) candidate.visible_count += 1U;
    }
    vps_column_set_reset(columns);
    *columns = candidate;
    return VPS_METADATA_OK;

invalid:
    vps_column_set_reset(&candidate);
    return VPS_METADATA_INVALID_RESULT;
}

VpsMetadataResult vps_column_set_string(
    const VpsColumnSet *columns,
    const VpsMetadataString *string,
    const unsigned char **value,
    size_t *length)
{
    if (columns == NULL || !columns->initialized || string == NULL ||
        value == NULL || length == NULL || !string->present ||
        string->offset > columns->text.size ||
        string->length > columns->text.size - string->offset)
        return VPS_METADATA_INVALID_ARGUMENT;
    *value = columns->text.data + string->offset;
    *length = string->length;
    return VPS_METADATA_OK;
}

void vps_column_set_reset(VpsColumnSet *columns)
{
    VpsAllocator allocator;
    VpsLogger *logger;
    int initialized;
    if (columns == NULL) return;
    allocator = columns->allocator;
    logger = columns->logger;
    initialized = columns->initialized;
    if (initialized && vps_allocator_is_valid(&allocator))
        vps_memory_release(&allocator, (void **)&columns->columns,
                           columns->columns_bytes);
    vps_buffer_reset(&columns->text);
    (void)memset(columns, 0, sizeof(*columns));
    if (initialized && vps_allocator_is_valid(&allocator)) {
        columns->allocator = allocator;
        columns->logger = logger;
        (void)vps_buffer_init(&columns->text, &allocator,
                              VPS_METADATA_MAX_TOTAL_BYTES);
        columns->initialized = 1;
    }
}

const char *vps_metadata_result_name(VpsMetadataResult result)
{
    switch (result) {
        case VPS_METADATA_OK: return "ok";
        case VPS_METADATA_INVALID_ARGUMENT: return "invalid_argument";
        case VPS_METADATA_INVALID_STATE: return "invalid_state";
        case VPS_METADATA_INVALID_RESULT: return "invalid_result";
        case VPS_METADATA_LIMIT_EXCEEDED: return "limit_exceeded";
        case VPS_METADATA_OUT_OF_MEMORY: return "out_of_memory";
        case VPS_METADATA_NOT_FOUND: return "not_found";
        case VPS_METADATA_UNSUPPORTED: return "unsupported";
        default: return "unknown";
    }
}

const char *vps_catalog_query_name(VpsCatalogQuery query)
{
    switch (query) {
        case VPS_CATALOG_QUERY_RELATION: return "catalog_relation";
        case VPS_CATALOG_QUERY_COLUMNS: return "catalog_columns";
        case VPS_CATALOG_QUERY_KEYS: return "catalog_keys";
        case VPS_CATALOG_QUERY_RELATION_POLICY: return "catalog_policy";
        case VPS_CATALOG_QUERY_POSTGIS: return "catalog_postgis";
        case VPS_CATALOG_QUERY_RELATIONS_FUNCTION: return "metadata_relations";
        case VPS_CATALOG_QUERY_TABLE_INFO_FUNCTION: return "metadata_table_info";
        case VPS_CATALOG_QUERY_INDEX_LIST_FUNCTION: return "metadata_index_list";
        case VPS_CATALOG_QUERY_INDEX_INFO_FUNCTION: return "metadata_index_info";
        case VPS_CATALOG_QUERY_TYPE_INFO_FUNCTION: return "metadata_type_info";
        case VPS_CATALOG_QUERY_EXTENSIONS_FUNCTION: return "metadata_extensions";
        default: return "catalog_unknown";
    }
}

const char *vps_relation_kind_name(VpsRelationKind kind)
{
    switch (kind) {
        case VPS_RELATION_TABLE: return "table";
        case VPS_RELATION_PARTITIONED_TABLE: return "partitioned_table";
        case VPS_RELATION_VIEW: return "view";
        case VPS_RELATION_MATERIALIZED_VIEW: return "materialized_view";
        case VPS_RELATION_FOREIGN_TABLE: return "foreign_table";
        case VPS_RELATION_INHERITANCE_PARENT: return "inheritance_parent";
        case VPS_RELATION_PARTITION: return "partition";
        case VPS_RELATION_SEQUENCE: return "sequence";
        case VPS_RELATION_COMPOSITE: return "composite";
        case VPS_RELATION_TOAST: return "toast";
        case VPS_RELATION_INDEX: return "index";
        case VPS_RELATION_UNSUPPORTED: return "unsupported";
        default: return "unknown";
    }
}
