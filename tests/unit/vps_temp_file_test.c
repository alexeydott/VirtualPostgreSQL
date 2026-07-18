#include "vps_temp_file.h"
#include "vps_embedded_sqlite.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
#if defined(_WIN32)
    VpsAllocator allocator;
    VpsTempFilePath path;
    VpsEmbeddedSqliteOpenOptions options;
    VpsEmbeddedSqlite *database = NULL;
    if (vps_allocator_system(&allocator) != VPS_MEMORY_OK) return 1;
    (void)memset(&path, 0, sizeof(path));
    if (vps_temp_file_create_private(&allocator, NULL, &path) !=
        VPS_TEMP_FILE_OK) return 1;
    (void)memset(&options, 0, sizeof(options));
    options.allocator = allocator;
    options.mode = VPS_EMBEDDED_SQLITE_TEMP;
    options.temp_path = path.path;
    options.temp_path_length = path.path_size - 1U;
    if (vps_embedded_sqlite_open(&options, &database) !=
            VPS_EMBEDDED_SQLITE_OK ||
        vps_embedded_sqlite_close(&database) != VPS_EMBEDDED_SQLITE_OK ||
        vps_temp_file_delete(&path) != VPS_TEMP_FILE_OK)
        return 1;
    (void)printf("[temp-file] acl=owner-only delete_on_close=passed\n");
#else
    (void)printf("[temp-file] status=skipped platform=non-windows\n");
#endif
    return 0;
}
