# argus-sqlite

The SQLite substrate: the Drogon client seam, the vec0 vector-index
repository, the schema runner and the small SQL helpers (prepared-statement
RAII, literal escaping).

## What this is

A PACKAGE, not a service: it opens no database of its own and applies no
schema of its own — the owning service's `database/schema.sql` is what runs
through it (rule 26). Ten units outside `packages/lib` link it — audit,
identity, memory, sync and six services.

It is one of the two libs declared as a standalone project
(`project(argus-sqlite)`), because the vendored `sqlite-vec` extension and
the `identity-client-test` suite both need a configure that stands on its
own.

## Layout

- `src/sqlite/db-service.{cc,hxx}` — `DbService`: `client()` for the host's own
  database; the named clients a service installs at boot (`readOnlyClient`,
  `identityClient`, `cameraClient`, `productivityClient`, `gatewayClient`,
  each with its `set*` installer); `enableUriFilenames`, `runScriptFile`,
  `applyPragmas`, `installExtensions`.
- `src/sqlite/vec-db.{cc,hxx}` — `VecDb`: the singleton handle onto the vec
  database plus the mutex that serialises it.
- `src/sqlite/schema-runner.{cc,hxx}` — `runSchemaFile`: executes every
  statement of a schema file, logging and skipping the ones that fail.
- `src/sqlite/sqlite-stmt.hxx` — `SqliteStmt`: move-only RAII over
  `sqlite3_stmt` (always finalized) and the blob binding it needs.
- `src/sqlite/sql-escape.hxx` — `sql_util::escapeLiteral`.
- `src/sqlite/details/` — private by convention: `vector-index-query.hxx`,
  `vector-index-repository.{cc,hxx}`. The vec0 DDL and the reconcile checks
  (`createTables`, `dropTables`, `recreateMemoryVec`, `recreateVecTables`,
  `tableSql`, `hasCosineMetric`, `memoryVecMatches`) are the package's
  internals, not a surface a service calls.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME sqlite ...)`
  declaration, explicit source lists, never `file(GLOB)`. The include root is
  `src/`, so consumers write `<sqlite/db-service.hxx>` and
  `<sqlite/sqlite-stmt.hxx>`.
- A schema is applied, never invented here: `runSchemaFile` takes the path the
  service resolved, and the DDL of a domain lives in that domain's
  `database/schema.sql` (rule 26). The one exception is the vec0 tables, whose
  shape is the extension's, and they are built by the repository above.
- Opening another domain's database directly is rule 27's exception, not the
  default: it happens through the named client the host installs at boot. The
  two families fail differently, and the difference is load-bearing — the
  identity, camera, productivity and gateway clients fall back to the host's
  own database, while `readOnlyClient` does NOT: uninstalled there means the
  tables it serves do not exist at all, and the repositories answer empty
  rather than opening something else in their place.
- `details/` is private by convention — nothing outside this package includes
  `sqlite/details/...`, and a service that needs a vec query asks the
  repository the domain owns, not this one.

## Tests

`tests/unit/identity-client-test.cc` — a read-only identity client installed
at boot serves the identity database and refuses a write.
