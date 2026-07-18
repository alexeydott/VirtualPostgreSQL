[← Previous: Query sources](query-sources.md) · [Back to README](../README.md)

# Read-only Virtual Table path

`VirtualPostgreSQL` now exposes the first end-to-end read path. `xCreate` and
`xConnect` parse strict module arguments, resolve a connection without retaining
the input connection string in SQLite schema output, connect immediately, and
prepare/describe a bounded table or query `SELECT`. SQLite schema declaration is
derived from the server result descriptor; a failed connection or descriptor
never falls back to a dummy schema.

`xFilter` opens an independent async client connection and starts an extended
protocol statement in single-row mode. `xNext` publishes one cursor-owned
decoded row at a time and propagates terminal or late PostgreSQL errors. The
borrowed libpq result stays valid until the next step or close. Early close
securely cancels and drains when bounded cleanup is possible; otherwise the
connection is destroyed. Predicate, projection, ordering, limit and offset
pushdown are described in the Stage 8 planner guide.

## Values and identity

NULL remains distinct from empty text and empty BLOB. Boolean and signed integer
values are range checked, finite floats become SQLite REAL, PostgreSQL special
float values remain TEXT, numeric values remain exact TEXT, and hex `bytea` is
decoded to exact BLOB bytes. Date/time, JSON, array, domain, enum and unknown
read-only values retain their bounded server text representation; UUID text must
be canonical lowercase form.

An explicit single integer `key_columns` value is used as stable SQLite rowid.
Other explicit keys expose a versioned, length-prefixed hidden identity token.
Sources without a stable key use a monotonically increasing scan-local rowid;
that value has no DML or cross-scan meaning.

## Diagnostics and testing

Runtime `VPS_LOG_LEVEL=debug` enables bounded structured server primary messages.
Raw SQL remains available only in a `VPS_DEBUG` build through the centralized
debug logger field, and values and credentials are never logged.

The Stage 7 Windows gate builds MSVC Win32 Debug, MSVC x64 Release, clang-cl x64
Debug and clang-cl x64 ASan. Its optional local contour reads a catalog table,
catalog view and validated query, including NULL, `bytea`, UUID, stable rowid and
two interleaved cursors. Connection details are supplied only through process
environment variables.

Stage 9 extends this contour with explicit host cancellation, checked scan
limits, late-error and early-close probes, bounded-RSS streaming and eight-way
cursor concurrency. See [Streaming and cancellation](streaming-cancellation.md).
