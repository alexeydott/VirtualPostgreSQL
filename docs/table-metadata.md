[← Previous: Client runtime](client-runtime.md) · [Back to README](../README.md) · [Next: Type mapping →](type-mapping.md)

# PostgreSQL table metadata

Stage 5 resolves a table-like source through bounded, schema-qualified
`pg_catalog` queries. Catalog filters use typed parameters; identifiers and
values are never concatenated into SQL.

## Metadata pipeline

| Step | Result | Safety property |
|---|---|---|
| Resolve relation | Namespace OID, relation OID and relation kind | Exact schema/name match; 0 or multiple rows fail |
| Decode columns | Ordered physical columns, types, domains and collations | Dropped columns remain fingerprint inputs but are not visible |
| Select codecs | Versioned codec and capability flags | Catalog names drive selection; unknown types use conservative text |
| Discover key | Primary, eligible unique, explicit validated key or none | Partial, expression, deferrable and invalid indexes are rejected |
| Evaluate policy | Partition, inheritance and row-level security flags | Unsafe inheritance or unproven keys become read-only |
| Fingerprint | Versioned 256-bit canonical fingerprint | Fixed-width serialization is independent of C padding and pointers |

The portable owners are `VpsMetadataRowSet`, `VpsRelationMetadata` and
`VpsColumnSet`. Each copies borrowed client rows through a checked allocator,
publishes only a complete candidate and provides idempotent reset. PostgreSQL
handles and `PQ*` calls remain inside `vps_libpq_client*`.

## Drift classification

The fingerprint includes relation identity, ordered semantic column metadata,
key eligibility, partition/inheritance/row-level-security policy, codec registry
version and a spatial capability placeholder. A comparison reports one bounded
change class without exposing catalog text.

Statistics targets and comments are deliberately excluded. Default/generated
expressions contribute only a server-side digest; their SQL text is neither
copied into the fingerprint nor logged.

## Verification

`scripts/ci/run-stage5.ps1` runs the four Windows compiler contours and metadata
source-boundary checks. A live catalog probe can additionally target a local or
TLS PostgreSQL fixture through process-local `VPS_SESSION_TEST_*` or
`VPS_TLS_TEST_*` variables. It queries catalogs only and prints OIDs, counts,
policy classes and the fingerprint.

## See Also

- [Client runtime](client-runtime.md) — backend-neutral statement and row lifetime.
- [Connection credentials](connection-credentials.md) — secure runtime configuration.
