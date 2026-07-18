#ifndef VPS_TEMP_FILE_H
#define VPS_TEMP_FILE_H

#include "vps_logging.h"
#include "vps_memory.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_TEMP_FILE_PATH_LIMIT 32768U

typedef enum VpsTempFileStatus {
    VPS_TEMP_FILE_OK = 0,
    VPS_TEMP_FILE_INVALID_ARGUMENT = 1,
    VPS_TEMP_FILE_OUT_OF_MEMORY = 2,
    VPS_TEMP_FILE_UNSUPPORTED = 3,
    VPS_TEMP_FILE_SYSTEM_ERROR = 4
} VpsTempFileStatus;

typedef struct VpsTempFilePath {
    VpsAllocator allocator;
    char *path;
    size_t path_size;
    uint64_t fingerprint;
} VpsTempFilePath;

VpsTempFileStatus vps_temp_file_create_private(
    const VpsAllocator *allocator,
    VpsLogger *logger,
    VpsTempFilePath *path);
VpsTempFileStatus vps_temp_file_delete(VpsTempFilePath *path);

#endif
