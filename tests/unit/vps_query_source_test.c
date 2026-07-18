#include "vps_query_source.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "CHECK failed at line %d: %s\n", \
                      __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static void expect(const char *sql, VpsQuerySourceResult expected)
{
    VpsQuerySourceAnalysis analysis;
    VpsQuerySourceResult result = vps_query_source_scan(
        sql, strlen(sql), NULL, &analysis);
    if (result != expected) {
        (void)fprintf(stderr, "unexpected result: got=%s expected=%s\n",
                      vps_query_source_result_name(result),
                      vps_query_source_result_name(expected));
        ++failures;
    }
}

int main(void)
{
    VpsQuerySourceAnalysis left;
    VpsQuerySourceAnalysis right;
    static const char nul_query[] = {'S','E','L','E','C','T',' ',0,'1'};
    static const unsigned char invalid_utf8[] = {
        'S','E','L','E','C','T',' ',UINT8_C(0xc0)
    };

    expect("SELECT 1", VPS_QUERY_SOURCE_OK);
    expect(" /* nested /* comment */ ok */ WITH x AS (SELECT 1) SELECT * FROM x; ",
           VPS_QUERY_SOURCE_OK);
    expect("SELECT $$; UPDATE t SET x=1$$ AS body", VPS_QUERY_SOURCE_OK);
    expect("SELECT $tag$FOR UPDATE; $1$tag$ AS body", VPS_QUERY_SOURCE_OK);
    expect("SELECT \"update\" FROM \"delete\"", VPS_QUERY_SOURCE_OK);
    expect("SELECT 1; SELECT 2", VPS_QUERY_SOURCE_MULTIPLE_STATEMENTS);
    expect("INSERT INTO t VALUES (1)", VPS_QUERY_SOURCE_UNSUPPORTED_ROOT);
    expect("WITH x AS (DELETE FROM t RETURNING *) SELECT * FROM x",
           VPS_QUERY_SOURCE_DATA_MODIFYING_CTE);
    expect("WITH x AS (SELECT 1) UPDATE t SET x=1",
           VPS_QUERY_SOURCE_FORBIDDEN_COMMAND);
    expect("SELECT * FROM t FOR UPDATE", VPS_QUERY_SOURCE_LOCKING_SELECT);
    expect("SELECT * FROM t FOR SHARE", VPS_QUERY_SOURCE_LOCKING_SELECT);
    expect("SELECT * INTO TEMP x FROM t", VPS_QUERY_SOURCE_SELECT_INTO);
    expect("SELECT $1", VPS_QUERY_SOURCE_UNRESOLVED_PARAMETER);
    expect("COPY t TO STDOUT", VPS_QUERY_SOURCE_UNSUPPORTED_ROOT);
    expect("CALL p()", VPS_QUERY_SOURCE_UNSUPPORTED_ROOT);
    expect("DO $$BEGIN END$$", VPS_QUERY_SOURCE_UNSUPPORTED_ROOT);
    expect("SELECT 'unterminated", VPS_QUERY_SOURCE_UNTERMINATED);
    expect("SELECT (1", VPS_QUERY_SOURCE_UNBALANCED);

    CHECK(vps_query_source_scan(nul_query, sizeof(nul_query), NULL, &left) ==
          VPS_QUERY_SOURCE_CONTAINS_NUL);
    CHECK(vps_query_source_scan((const char *)invalid_utf8,
                                sizeof(invalid_utf8), NULL,
                                &left) == VPS_QUERY_SOURCE_INVALID_UTF8);
    CHECK(vps_query_source_scan("SELECT  1 -- ignored\n", 21U, NULL, &left) ==
          VPS_QUERY_SOURCE_OK);
    CHECK(vps_query_source_scan("select 1", 8U, NULL, &right) ==
          VPS_QUERY_SOURCE_OK);
    CHECK(left.normalized_hash == right.normalized_hash);
    CHECK(left.format_version == VPS_QUERY_SOURCE_FORMAT_VERSION);
    CHECK(vps_query_source_scan("SELECT А", sizeof("SELECT А") - 1U,
                                NULL, &left) == VPS_QUERY_SOURCE_OK);
    {
        uint64_t cyrillic_hash = left.normalized_hash;
        CHECK(vps_query_source_scan("SELECT Б", sizeof("SELECT Б") - 1U,
                                    NULL, &left) == VPS_QUERY_SOURCE_OK);
        CHECK(left.normalized_hash != cyrillic_hash);
    }

    if (failures != 0) {
        (void)fprintf(stderr, "vps_query_source_test: %d failure(s)\n", failures);
        return 1;
    }
    (void)puts("vps_query_source_test: passed");
    return 0;
}
