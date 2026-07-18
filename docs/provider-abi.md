[← Previous: Connection and credentials](connection-credentials.md) · [Back to README](../README.md) · [Next: Security →](security.md)

# Credential provider ABI 1.0

Public C ABI находится только в `include/virtualpostgresql/vps_api.h`.
Версия кодируется как `major.minor.patch`; Windows 1.0 использует
`VPS_API_VERSION_ENCODE(1,0,0)` и calling convention `__cdecl`.

Host заполняет `VpsCredentialProvider`:

1. `header.structure_size`, `header.api_version` и `present_fields`;
2. thread-safe `resolve(context, ref, length, lease)`;
3. `release(context, lease)`, вызываемый ровно один раз после каждого
   успешного `resolve`, включая последующую validation/OOM failure.

Provider-owned UTF-8 strings действуют только до `release`. Extension сначала
проверяет declared size/version/field masks/lengths, затем копирует значения в
owned checked buffers и secure-zero secrets при cleanup. Callback не должен
вызывать SQLite API и не должен возвращать pointers на stack storage.

Larger compatible structures принимаются через prefix contract; неизвестная
major version и слишком короткий required prefix отклоняются. Exact runtime
sizes можно получить exported functions
`virtualpostgresql_credential_*_structure_size()`.

Минимальный host example: [examples/credential-provider.c](../examples/credential-provider.c).

## See Also

- [Connection and credentials](connection-credentials.md) — выбор mode.
- [Security](security.md) — secret lifetime и redaction.
- [Platform support](platform-support.md) — Windows Credential Manager adapter.
