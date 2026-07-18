[← Previous: Connection and credentials](connection-credentials.md) · [Back to README](../README.md) · [Next: Security →](security.md)

# Credential provider ABI 1.0

The public C ABI is defined only in `include/virtualpostgresql/vps_api.h`.
Versions are encoded as `major.minor.patch`; Windows 1.0 uses
`VPS_API_VERSION_ENCODE(1,0,0)` and the `__cdecl` calling convention.

The host initializes `VpsCredentialProvider`:

1. `header.structure_size`, `header.api_version`, and `present_fields`;
2. a thread-safe `resolve(context, ref, length, lease)` callback;
3. `release(context, lease)`, called exactly once after each successful
   `resolve`, including a subsequent validation or out-of-memory failure.

Provider-owned UTF-8 strings remain valid only until `release`. The extension
first validates declared sizes, versions, field masks, and lengths; it then
copies values into checked owned buffers and securely zeros secrets during
cleanup. A callback must not invoke the SQLite API or return pointers to stack
storage.

Larger compatible structures are accepted through the prefix contract. An
unknown major version or a structure shorter than the required prefix is
rejected. Exact runtime sizes are available from the exported functions
`virtualpostgresql_credential_*_structure_size()`.

Register a provider after loading the extension with
`virtualpostgresql_register_credential_provider(database, &provider)`. The
registration is scoped to that SQLite connection and may replace the default
provider only before the first credential resolution. On Windows,
`virtualpostgresql_wincred_provider(&provider)` initializes the built-in
Windows Generic Credential adapter for explicit host registration. Both
functions return SQLite result codes.

Minimal host example: [examples/credential-provider.c](../examples/credential-provider.c).

## See Also

- [Connection and credentials](connection-credentials.md) — mode selection.
- [Security](security.md) — secret lifetime and redaction.
- [Platform support](platform-support.md) — Windows Credential Manager adapter.
