#include "vps_embedded_sqlite.h"
#include "vps_platform.h"

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

#define VPS_PERF_SAMPLES 3U
#define VPS_PERF_PROBES 100U
#define VPS_PERF_ROWS 256U

static int build_snapshot(const VpsAllocator *allocator,
                          VpsEmbeddedSqlite **database)
{
    VpsEmbeddedSqliteOpenOptions options;
    VpsEmbeddedValueKind kind = VPS_EMBEDDED_VALUE_INTEGER;
    VpsEmbeddedIndexDefinition index;
    VpsEmbeddedSchema schema;
    VpsEmbeddedValue value;
    size_t row;
    (void)memset(&options, 0, sizeof(options));
    options.allocator = *allocator;
    options.mode = VPS_EMBEDDED_SQLITE_MEMORY;
    if (vps_embedded_sqlite_open(&options, database) !=
        VPS_EMBEDDED_SQLITE_OK) return 0;
    (void)memset(&index, 0, sizeof(index));
    index.columns[0] = 0U;
    index.column_count = 1U;
    (void)memset(&schema, 0, sizeof(schema));
    schema.column_kinds = &kind;
    schema.column_count = 1U;
    schema.indexes = &index;
    schema.index_count = 1U;
    if (vps_embedded_sqlite_create_schema(*database, &schema) !=
        VPS_EMBEDDED_SQLITE_OK) return 0;
    (void)memset(&value, 0, sizeof(value));
    value.kind = VPS_EMBEDDED_VALUE_INTEGER;
    for (row = 0U; row < VPS_PERF_ROWS; ++row) {
        value.integer = (int64_t)row;
        if (vps_embedded_sqlite_append_row(*database, &value, 1U) !=
            VPS_EMBEDDED_SQLITE_OK) return 0;
    }
    return vps_embedded_sqlite_seal(*database) == VPS_EMBEDDED_SQLITE_OK;
}

static int probe(VpsEmbeddedSqlite *database, int64_t key)
{
    uint16_t projection = 0U;
    VpsEmbeddedConstraint constraint;
    VpsEmbeddedScanRequest request;
    VpsEmbeddedSqliteScan *scan = NULL;
    int has_row = 0;
    (void)memset(&constraint, 0, sizeof(constraint));
    constraint.column = 0U;
    constraint.operation = VPS_EMBEDDED_OP_EQ;
    constraint.value.kind = VPS_EMBEDDED_VALUE_INTEGER;
    constraint.value.integer = key;
    (void)memset(&request, 0, sizeof(request));
    request.projection = &projection;
    request.projection_count = 1U;
    request.constraints = &constraint;
    request.constraint_count = 1U;
    request.use_index = 1;
    request.selected_index = 0U;
    if (vps_embedded_sqlite_scan_open(database, &request, &scan) !=
            VPS_EMBEDDED_SQLITE_OK ||
        !vps_embedded_sqlite_scan_uses_index(scan) ||
        vps_embedded_sqlite_scan_step(scan, &has_row) !=
            VPS_EMBEDDED_SQLITE_OK || !has_row) {
        (void)vps_embedded_sqlite_scan_close(&scan);
        return 0;
    }
    return vps_embedded_sqlite_scan_close(&scan) == VPS_EMBEDDED_SQLITE_OK;
}

static uint64_t median3(uint64_t values[3])
{
    if (values[0] > values[1]) { uint64_t t=values[0]; values[0]=values[1]; values[1]=t; }
    if (values[1] > values[2]) { uint64_t t=values[1]; values[1]=values[2]; values[2]=t; }
    if (values[0] > values[1]) { uint64_t t=values[0]; values[0]=values[1]; values[1]=t; }
    return values[1];
}

static uint64_t percentile95_3(const uint64_t values[3])
{
    uint64_t maximum = values[0];
    if (values[1] > maximum) maximum = values[1];
    if (values[2] > maximum) maximum = values[2];
    return maximum;
}

static uint64_t peak_rss_bytes(void)
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters;
    (void)memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                             sizeof(counters)) != 0)
        return (uint64_t)counters.PeakWorkingSetSize;
#endif
    return 0U;
}

int main(void)
{
    VpsAllocator allocator;
    const VpsPlatformOperations *platform = vps_platform_current_operations();
    uint64_t off_ms[VPS_PERF_SAMPLES];
    uint64_t memory_ms[VPS_PERF_SAMPLES];
    uint64_t sample;
    uint64_t off_p50;
    uint64_t memory_p50;
    uint64_t off_p95;
    uint64_t memory_p95;
    if (vps_allocator_system(&allocator) != VPS_MEMORY_OK) return 1;
    for (sample = 0U; sample < VPS_PERF_SAMPLES; ++sample) {
        uint64_t start;
        uint64_t end;
        size_t probe_index;
        VpsEmbeddedSqlite *snapshot = NULL;
        if (vps_platform_monotonic_now_ms(platform, &start) != VPS_PLATFORM_OK)
            return 1;
        for (probe_index = 0U; probe_index < VPS_PERF_PROBES; ++probe_index) {
            VpsEmbeddedSqlite *remote = NULL;
            if (!build_snapshot(&allocator, &remote) ||
                !probe(remote, (int64_t)(probe_index % VPS_PERF_ROWS)) ||
                vps_embedded_sqlite_close(&remote) != VPS_EMBEDDED_SQLITE_OK)
                return 1;
        }
        if (vps_platform_monotonic_now_ms(platform, &end) != VPS_PLATFORM_OK)
            return 1;
        off_ms[sample] = end - start;
        if (!build_snapshot(&allocator, &snapshot) ||
            vps_platform_monotonic_now_ms(platform, &start) != VPS_PLATFORM_OK)
            return 1;
        for (probe_index = 0U; probe_index < VPS_PERF_PROBES; ++probe_index)
            if (!probe(snapshot, (int64_t)(probe_index % VPS_PERF_ROWS)))
                return 1;
        if (vps_platform_monotonic_now_ms(platform, &end) != VPS_PLATFORM_OK ||
            vps_embedded_sqlite_close(&snapshot) != VPS_EMBEDDED_SQLITE_OK)
            return 1;
        memory_ms[sample] = end - start;
    }
    off_p95 = percentile95_3(off_ms);
    memory_p95 = percentile95_3(memory_ms);
    off_p50 = median3(off_ms);
    memory_p50 = median3(memory_ms);
    (void)printf("[materialization] samples=%u probes=%u executions_off=%u "
                 "executions_memory=1 off_p50_ms=%llu memory_p50_ms=%llu "
                 "off_p95_ms=%llu memory_p95_ms=%llu rss_peak_bytes=%llu "
                 "environment=private-sqlite-local options=%llu status=%s\n",
                 VPS_PERF_SAMPLES, VPS_PERF_PROBES, VPS_PERF_PROBES,
                 (unsigned long long)off_p50,
                 (unsigned long long)memory_p50,
                 (unsigned long long)off_p95,
                 (unsigned long long)memory_p95,
                 (unsigned long long)peak_rss_bytes(),
                 (unsigned long long)
                     vps_embedded_sqlite_compile_options_fingerprint(),
                 off_p50 != 0U && memory_p50 * 10U <= off_p50 * 9U
                     ? "passed" : "failed");
    return off_p50 != 0U && memory_p50 * 10U <= off_p50 * 9U ? 0 : 1;
}
