#include "sqlite3.h"

#include <stdio.h>

int vps_extension_check_host_version(int version_number);

int main(void)
{
    int passed = 1;

    passed &= vps_extension_check_host_version(3043999) == SQLITE_ERROR;
    passed &= vps_extension_check_host_version(3044000) == SQLITE_OK;
    passed &= vps_extension_check_host_version(3050300) == SQLITE_OK;
    (void)printf(
        "[host-version] level=info minimum=3044000 negative=3043999 "
        "status=%s\n",
        passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
