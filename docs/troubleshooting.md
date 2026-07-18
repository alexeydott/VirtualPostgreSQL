[← Previous: Sanitizers](sanitizers.md) · [Back to README](../README.md) · [Next: Platform support →](platform-support.md)

# Troubleshooting

| Symptom | Check | Safe action |
|---|---|---|
| DLL does not load | SQLite ≥3.44 and matching x86/x64 architecture | run `test-windows-binary.ps1` |
| `authentication failed` | role/provider/service | update the secret source; do not print connstr |
| TLS/certificate error | hostname, CA, `sslmode` | repair trust; do not lower the mode automatically |
| `SQLITE_SCHEMA` | relation OID/fingerprint drift | recreate, or use compatible `schema_policy=refresh` |
| `SQLITE_BUSY` | active stream/transaction participant | close the cursor and retry at application level |
| `SQLITE_INTERRUPT` | deadline or cancellation | inspect timeout and host interrupt policy |
| commit outcome unknown | connection lost during COMMIT | treat as failure; reconcile by durable business key |
| PostGIS unavailable | extension/schema discovery | install/grant PostGIS or remove the spatial mode |

In a debug build, the server primary error passes bounded redaction. SQL can be
enabled only by combining `VPS_DEBUG` with runtime `debug`; values and conninfo
still are not logged. Do not attach environment dumps, database URLs,
certificates/private keys, row data, or SQLite schemas containing `connstr` to
a bug report.

For a reproducible report, provide the architecture, SQLite version,
PostgreSQL major version, extension API version, operation/phase, SQLite code,
and SQLSTATE. Then run the appropriate gate from [Building](building.md).

## See Also

- [Security](security.md) — permitted diagnostic fields.
- [Building](building.md) — local verification commands.
- [Transactions](transactions-savepoints.md) — ambiguous COMMIT recovery.
