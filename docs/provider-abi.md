[← Previous: Connection and credentials](connection-credentials.md) · [Back to README](../README.md) · [Next: Security →](security.md)

# Current credential provider ABI

The public C ABI is defined only in `include/virtualpostgresql/vps_api.h`.
Currently, compatibility is encoded by the public `VPS_API_VERSION` constants,
and Windows callbacks use the `__cdecl` calling convention.

The host initializes `VpsCredentialProvider`:

1. `header.structure_size`, `header.api_version`, and `present_fields`;
2. a thread-safe `resolve(context, ref, length, lease)` callback;
3. `release(context, lease)`, called exactly once after each successful
   `resolve`, including a subsequent validation or out-of-memory failure.

Provider-owned UTF-8 strings remain valid only until `release`. The extension
first validates declared sizes, API compatibility, field masks, and lengths; it then
copies values into checked owned buffers and securely zeros secrets during
cleanup. A callback must not invoke the SQLite API or return pointers to stack
storage.

Larger compatible structures are accepted through the prefix contract. An
incompatible major API identifier or a structure shorter than the required prefix is
rejected. Exact runtime sizes are available from the exported functions
`virtualpostgresql_credential_*_structure_size()`.

Register a provider after loading the extension with
`virtualpostgresql_register_credential_provider(database, &provider)`. The
registration is scoped to that SQLite connection and may replace the default
provider only before the first credential resolution. On Windows,
`virtualpostgresql_wincred_provider(&provider)` initializes the built-in
Windows Generic Credential adapter for explicit host registration. Both
functions return SQLite result codes.

## Query-profile providers

`VpsQueryProfileProvider` resolves a bounded UTF-8 profile name into a
`VpsQueryProfileLease`. A successful lease supplies one length-delimited query
and a nonzero revision. The extension copies and validates the query, then
calls `release` exactly once, including allocation or validation failures.

Register a provider for one source slot and one loaded SQLite connection with
`virtualpostgresql_register_query_profile_provider()`. Callbacks and
`provider_context` remain host-owned until that SQLite connection closes.
Providers may be called concurrently. Replacement of a source slot is allowed
only before its first resolve attempt. Runtime structure sizes are available
through the two `virtualpostgresql_query_profile_*_structure_size()` exports.

Host examples:

- [generic credential provider](../examples/credential-provider.c);
- [Windows Credential Manager adapter](../examples/windows-credential-provider.c);
- [query-profile provider](../examples/query-profile-provider.c).

## See Also

- [Connection and credentials](connection-credentials.md) — mode selection.
- [Security](security.md) — secret lifetime and redaction.
- [Platform support](platform-support.md) — Windows Credential Manager adapter.
