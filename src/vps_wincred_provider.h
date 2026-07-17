#ifndef VPS_WINCRED_PROVIDER_H
#define VPS_WINCRED_PROVIDER_H

#include "vps_credential_provider.h"

#include <stddef.h>
#include <stdint.h>

/*
 * CredentialBlob is a bounded little-endian VPSC record:
 *   u32 magic, u16 version, u16 field_count;
 *   repeated { u64 VPS_CREDENTIAL_FIELD_*, u32 UTF-8 length, u32 zero, bytes }.
 * Entries must be unique, known, control-free UTF-8 and fill the blob exactly.
 */
#define VPS_WINCRED_FORMAT_MAGIC UINT32_C(0x43535056)
#define VPS_WINCRED_FORMAT_VERSION UINT16_C(1)
#define VPS_WINCRED_FORMAT_HEADER_SIZE 8U
#define VPS_WINCRED_FORMAT_ENTRY_SIZE 16U
#define VPS_WINCRED_BLOB_MAX_LENGTH 2560U

typedef enum VpsWinCredReadResult {
    VPS_WINCRED_READ_OK = 0,
    VPS_WINCRED_READ_NOT_FOUND = 1,
    VPS_WINCRED_READ_ERROR = 2
} VpsWinCredReadResult;

/*
 * A successful read returns one mutable native record. blob remains owned by
 * the WinCred API until release_record. The adapter securely erases blob and
 * calls release_record exactly once on every successful read path.
 */
typedef struct VpsWinCredRecord {
    unsigned char *blob;
    size_t blob_size;
    void *native_record;
} VpsWinCredRecord;

typedef VpsWinCredReadResult (*VpsWinCredReadFunction)(
    void *context,
    const uint16_t *target,
    size_t target_length,
    VpsWinCredRecord *record);
typedef void (*VpsWinCredReleaseFunction)(void *context,
                                          VpsWinCredRecord *record);

typedef struct VpsWinCredApi {
    VpsWinCredReadFunction read;
    VpsWinCredReleaseFunction release_record;
} VpsWinCredApi;

/*
 * Context owns only its mutex. Allocator/API/platform/logger contexts are
 * borrowed and must outlive cleanup. Resolve is thread-safe; cleanup returns
 * BUSY while any resolve or returned lease is active and is otherwise
 * idempotent. Allocator and injected API callbacks must support concurrent use.
 */
typedef struct VpsWinCredProviderContext {
    VpsAllocator allocator;
    const VpsPlatformOperations *operations;
    VpsLogger *logger;
    VpsWinCredApi api;
    void *api_context;
    VpsPlatformMutex mutex;
    uint64_t active_resolves;
    uint64_t active_leases;
    int initialized;
} VpsWinCredProviderContext;

VpsCredentialRegistryResult vps_wincred_provider_init(
    VpsWinCredProviderContext *context,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger);
VpsCredentialRegistryResult vps_wincred_provider_init_with_api(
    VpsWinCredProviderContext *context,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger,
    const VpsWinCredApi *api,
    void *api_context);
VpsCredentialRegistryResult vps_wincred_provider_make(
    VpsWinCredProviderContext *context,
    VpsCredentialProvider *provider);
VpsCredentialRegistryResult vps_wincred_provider_cleanup(
    VpsWinCredProviderContext *context);

#endif
