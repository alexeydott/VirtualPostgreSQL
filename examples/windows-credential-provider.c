#include <virtualpostgresql/vps_api.h>

#if !defined(_WIN32)
#error "This example requires Windows Credential Manager support."
#endif

/*
 * The host resolves these two exports from the loaded DLL or links them from
 * an import library. Passing function pointers keeps this example independent
 * of the host's DLL-loading policy.
 */
typedef int(VPS_CALL *VpsInitializeWincredProviderFn)(
    VpsCredentialProvider *provider);
typedef int(VPS_CALL *VpsRegisterCredentialProviderFn)(
    sqlite3 *database,
    const VpsCredentialProvider *provider);

int register_windows_credential_provider(
    sqlite3 *database,
    VpsInitializeWincredProviderFn initialize_provider,
    VpsRegisterCredentialProviderFn register_provider)
{
    VpsCredentialProvider provider = {0};
    int result;

    if (database == NULL || initialize_provider == NULL ||
        register_provider == NULL) {
        return 21; /* SQLITE_MISUSE */
    }

    result = initialize_provider(&provider);
    if (result != 0) { /* SQLITE_OK */
        return result;
    }
    return register_provider(database, &provider);
}
