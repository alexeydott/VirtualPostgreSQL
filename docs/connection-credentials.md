[Back to README](../README.md) · [Next: Client runtime →](client-runtime.md)

# Connection credential modes

VirtualPostgreSQL accepts exactly one connection credential mode per virtual
table: `credential_ref`, `service`, `profile`, or `connstr`.

- Prefer `credential_ref` for secrets managed by a registered credential
  provider.
- Use `service` for a named libpq service. `service_file`, when supplied, must
  be an absolute, traversal-free path. The extension never logs the file or
  its resolved contents.
- Use `profile` for bounded `VPS_PROFILE_<NAME>_<FIELD>` environment values.
  Profile names are normalized to uppercase and `-`/`.` become `_`.
- Treat `connstr` as a compatibility mode. A persistent virtual table stores
  its CREATE statement, including the literal `connstr`, in `sqlite_schema`.
  Therefore a password or other secret embedded in `connstr` is visible to
  every principal and backup that can read that schema. Prefer
  `credential_ref` or `service`; never put a production password in a
  persistent `connstr`.

The `connstr` mode accepts a bounded keyword/value string. Unknown or repeated
keywords, URI syntax, `options`, uncontrolled `fallback_application_name`, and
embedded NUL bytes are rejected. Connection strings, environment values,
provider references, service-file contents, and effective libpq settings are
not emitted to logs or diagnostics.

## Canonical connection identity

Resolved connection settings are converted into a versioned, bounded canonical
identity. It includes normalized hosts and ports, database/user/service,
service and TLS paths, TLS/channel/target policies, controlled search path,
read/write class, and relevant timeouts. Exact pool and transaction matching
uses the canonical bytes; the stable fingerprint is for diagnostics only.

Passwords, tokens, raw connection strings, provider references and leases, key
contents, and environment variable names are excluded. Credential or service
configuration revisions are tracked as generations: changing only a password
keeps the same fingerprint but causes a generation mismatch so an older
connection cannot be reused as current configuration.

## TLS policy

The product default is `sslmode=verify-full` with
`channel_binding=prefer`. Supported TLS modes are `verify-full`, `verify-ca`,
`require`, `prefer`, and `disable`; channel binding accepts `prefer`,
`require`, or `disable`. Plaintext is available only when the caller explicitly
allows `sslmode=disable`. `disable` combined with required channel binding is
rejected before connection setup.

After a connection succeeds, the libpq boundary verifies that the effective
TLS and channel-binding settings still match the requested policy and checks
the actual SSL state. A successful `verify-full` or `verify-ca` connection is
the evidence that libpq completed hostname/CA validation; a mismatched policy
or missing required SSL fails closed. Diagnostics contain only the mode,
SSL-in-use flag, certificate-status class, channel-binding-status class, and
outcome. Effective connection strings, certificate paths or contents, and
private-key material are never logged.

The certificate-enabled integration probe makes seven connections and sends no
SQL queries. Supply its host, port, user, password, and database only through
the process-local `VPS_TLS_TEST_*` environment variables, build the Release
x64 contour, and run `scripts/ci/test-tls-stand.ps1`. The probe checks
`require`, `prefer`, valid `verify-full`, unrelated CA, mismatched hostname,
required channel binding, and explicit `disable` in one compact run. The local
no-SSL development stand is valid only for the explicit-disable contour and
does not replace positive certificate evidence.

For `channel_binding=require`, either a successful bound connection or an
explicit libpq channel-binding rejection is accepted evidence: the latter
proves fail-closed behavior on a server whose authentication contour does not
offer channel binding. Unclassified connection or authentication failures
still fail the gate.

For a manual run from `cmd.exe`, pass `-Interactive` to have the child
PowerShell process request missing fields. Password input is masked, used only
as a process-local environment value, and removed by the runner during cleanup.
For static OpenSSL builds, the runner uses `SSL_CERT_FILE` when already set or
discovers Git for Windows' CA bundle; `-TrustedCaFile` provides an explicit
trusted bundle when neither is available. The bundle path is never logged.

## Session baseline

Every newly connected or reset libpq session receives the same ordered
baseline before it can be published for use. The baseline sets and verifies
`client_encoding=UTF8`, ISO/YMD `DateStyle`, `IntervalStyle=iso_8601`, the
configured time zone (`UTC` by default), standard-conforming strings,
application name, controlled search path, statement/lock/idle-transaction
timeouts, and the read-only class. Internal catalog SQL remains explicitly
`pg_catalog`-qualified and does not depend on that search path.

Search-path components are bounded PostgreSQL identifiers. Unquoted names
use the portable identifier subset; quoted names may contain spaces and use
doubled quotes for an embedded quote. Separators or SQL text outside that
grammar are rejected before network access. Timeout inputs are bounded
unsigned milliseconds; server-reported equivalent units are normalized when
the result is checked.

Application is transactional at the plan boundary and fail-closed at the
connection boundary. The adapter requires an idle transaction state, disabled
pipeline mode, and no pending results before and after the ordered settings.
It stops on the first failed or mismatched setting, so a partially initialized
connection is never treated as reusable. Structured diagnostics contain only
the parameter name, expected-value class, connect/reset phase, and outcome;
setting values and full SQL are excluded.

The compact runtime probe applies an initial baseline, deliberately applies a
different valid baseline, then restores the initial baseline as a reset. It
uses one connection and 33 parameter-setting calls, performs no user-data
queries, and reads connection details only from process-local
`VPS_SESSION_TEST_*` environment variables. Explicit no-SSL evidence must run
with `VPS_SESSION_TEST_SSLMODE=disable`; a TLS run proves the same session
contract but does not replace that no-SSL gate.

## See Also

- [Client runtime](client-runtime.md) — async connection and cancellation contract.
- [Table metadata](table-metadata.md) — catalog access after session setup.
