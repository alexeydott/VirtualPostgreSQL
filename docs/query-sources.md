[← Previous: Type mapping](type-mapping.md) · [Back to README](../README.md) · [Next: Read-only Virtual Table →](read-only-vtable.md)

# Query sources

Query source отображает результат одного `SELECT` или `WITH ... SELECT` как read-only virtual table. Inline query проходит bounded lexical scanner, затем PostgreSQL prepare/describe внешнего wrapper. В production предпочтителен versioned `query_profile`, разрешаемый host/protected-config/environment/named-registry provider.

## Validation boundary

До выполнения extension:

1. проверяет UTF-8, NUL, размер, quotes/comments/dollar quotes и один statement;
2. отклоняет unresolved `$n`, DML/DDL, data-modifying CTE, `COPY`, `CALL`, `DO`, transaction/session control, `LISTEN`/`NOTIFY`, locking SELECT и `SELECT INTO`;
3. подготавливает и описывает без выполнения `SELECT * FROM (<query>) AS vps_validation LIMIT 0`;
4. требует непустые уникальные result aliases с ASCII case-insensitive canonical comparison;
5. разрешает `key_columns` и `query_indexes` только по canonical result columns;
6. выполняет scan внутри `BEGIN READ ONLY` с transaction-local controlled `search_path`, `statement_timeout`, `lock_timeout` и extension-side row/byte/deadline limits;
7. завершает boundary только `COMMIT` или `ROLLBACK`.

Inner query не переписывается. Будущий planner добавляет predicates, order и limits только во внешний execution wrapper.

## Security limitation

Query validation — defense in depth, а не PostgreSQL sandbox. Разрешённый `SELECT` способен вызвать функцию с внешними side effects, которые read-only transaction не отменяет. Production configuration должна использовать отдельную least-privilege role, schema-qualified object names, controlled `search_path`, отозванные unnecessary `EXECUTE` privileges и approved query profiles. Inline query требует явного предупреждения вызывающему приложению.

Raw query, profile content и values не входят в обычные logs/errors/fingerprints. Debug SQL разрешён только существующим compile-time `VPS_DEBUG` и runtime `debug` gate; Release/RelWithDebInfo отклоняют это поле.

## Result identity and indexes

`key_columns` объявляет validated logical identity только для read-only planning; query source не получает DML. `query_indexes` использует grammar `name=column[,column...][;...]`, сохраняет порядок columns и не заявляет remote/unique index. Unique planning может опираться только на validated `key_columns`.

Query fingerprint включает normalized query hash/profile version, ordered aliases and OID/typmod/origin/collation metadata, spatial policy, keys, query indexes, materialization mode и wrapper/codec versions. Изменение любого contract-relevant component считается metadata drift.

## Query materialization

`query_materialization=off|memory|temp` управляет lazy snapshot только для `source=query`; default `off` сохраняет single-row remote streaming. `memory` строит immutable private in-memory SQLite database, `temp` — private database в `%LOCALAPPDATA%\VirtualPostgreSQL\Temp` с owner-only ACL, in-memory rollback journal и delete-on-close cleanup. Host SQLite handles и allocators не передаются private SQLite.

Первый scan полностью выполняет validated source query один раз, переносит уже декодированные portable SQLite values и строит non-unique physical indexes из `query_indexes`. Candidate становится видимым только после terminal remote result, всех limit checks, local index build и private commit. Failed/late-error/OOM/temp/index build очищается без partial publish; следующий caller может безопасно повторить build. Published generation immutable, не имеет TTL, automatic refresh или host-shadow rows.

Subsequent cursors получают refcounted lease и выполняют exact predicates, projection, compatible ordering и consumed limit/offset локально. `query_indexes` никогда не создаёт unique claim: `SQLITE_INDEX_SCAN_UNIQUE` по-прежнему допускается только для полностью constrained validated `key_columns`. Для обновления snapshot virtual table нужно disconnect/reconnect или recreate; скрытого refresh нет.

Stage gate измеряет минимум 3 samples по 100 probes: remote executions должны сокращаться с `N` до `1`, physical local index подтверждается private `EXPLAIN QUERY PLAN`, а p50 должен улучшаться минимум на 10%. Bulk/equivalence/performance contour выполняется на локальном no-SSL стенде; quota-limited TLS stand не используется для bulk data.

## See Also

- [Read-only Virtual Table](read-only-vtable.md) — SQLite callback path.
- [Planner](planner-pushdown.md) — exact pushdown policy.
- [Type mapping](type-mapping.md) — result codecs.
