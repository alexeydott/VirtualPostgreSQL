#include "vps_query_validation.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    (void)fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #condition); \
    ++failures; } } while (0)

int main(void)
{
    VpsAllocator allocator;
    VpsQueryValidation validation;
    const VpsClientStatementSpec *spec;
    static const char query[] = "SELECT 1 AS id; -- terminal";

    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    CHECK(vps_query_validation_init(&validation, &allocator, query,
                                    sizeof(query) - 1U, 1000U, NULL) ==
          VPS_QUERY_VALIDATION_OK);
    spec = vps_query_validation_statement_spec(&validation);
    CHECK(spec != NULL);
    CHECK(spec->prepare == 1 && spec->discover_result_fields == 1);
    CHECK(strstr(spec->query, "SELECT * FROM (SELECT 1 AS id)") != NULL);
    CHECK(strstr(spec->query, "terminal") == NULL);
    CHECK(spec->parameters == NULL && spec->result_fields == NULL);
    vps_query_validation_cleanup(&validation);

    CHECK(vps_query_validation_init(&validation, &allocator,
                                    "DELETE FROM t", 13U, 1000U, NULL) ==
          VPS_QUERY_VALIDATION_SCAN_REJECTED);
    if (failures != 0) return 1;
    (void)puts("vps_query_validation_test: passed");
    return 0;
}
