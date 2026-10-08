# argus-sqlite

The SQLite substrate: the Drogon client seam, the vec0 vector-index
repository, the schema runner and the small SQL helpers (prepared-statement
RAII, literal escaping).

## What this is

A PACKAGE, not a service: it opens no database of its own and applies no
schema of its own — the owning service's `database/schema.sql` is what runs
through it (rule 26). Eight units outside `packages/lib` link it, all of
them services — auth, camera, guard, identity, llm, notification, productivity
and sync.

It is one of the two libs declared as a standalone project
(`project(argus-sqlite)`), because the vendored `sqlite-vec` extension and
the `identity-client-test` suite both need a configure that stands on its
own.

## Layout

- `src/sqlite/db-service.{cc,hxx}` — `DbService`: `client()` for the host's own
  database — Drogon's client until `freezeClient(dbPath)` arms the shutdown
  one, and that one from then on; the named clients a service installs at boot
  (`readOnlyClient`, `identityClient`, `cameraClient`, `productivityClient`,
  each with its `set*` installer); `enableUriFilenames`,
  `runScriptFile`, `applyPragmas`, `installExtensions`.
- `src/sqlite/vec-db.{cc,hxx}` — `VecDb`: the singleton handle onto the vec
  database plus the mutex that serialises it. `setDbFile` with a different
  file closes the open handle, so the next `handle()` opens that file: it
  used to keep the first file's connection, and a second store opened in the
  same process (argus-llm's memory suites) wrote its vectors and its
  forgets into the first one.
- `src/sqlite/schema-runner.{cc,hxx}` — `runSchemaFile`: executes every
  statement of a schema file, logging and skipping the ones that fail.
- `src/sqlite/table-rebuild.{cc,hxx}` — `table_rebuild::run`: rebuilds tables
  whose stored definition a callback rewrites (SQLite cannot drop a CHECK in
  place): backup first, foreign keys off outside the transaction, copy,
  compare counts, drop, rename, recreate indexes and triggers, keep the
  AUTOINCREMENT sequence, `foreign_key_check` before commit, keys back on. Its
  one consumer is identity's role CHECK removal.
- `src/sqlite/transaction.{cc,hxx}` — `db_transaction`: `begin` over a
  `DbClient`, the `Commit` awaiter a unit of work co_awaits (it resumes with
  whether the commit landed), `rollback`, and `CommitObserver`: an RAII
  registration whose callback runs after every commit that lands through
  `Commit`. An outbox row only becomes visible to its drain at that commit,
  so a drain woken from inside the transaction found nothing and slept its
  retry period; the observer wakes it at the moment the rows appear.
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
- The vendored `sqlite-vec` tree carries one local patch, recorded beside it
  (`third_party/sqlite-vec/argus-patches/README.md`,
  `0001-vec0-free-the-metadata-chunk-shadow-names.patch`): upstream
  `vec0_free()` frees every shadow table name `vec0_init()` allocates except
  `shadowMetadataChunksNames[]`, so every `vec0` table creation leaked two
  allocations of 40 bytes. `vec-db-test` is the measurement — its two tables
  (`memory_vec`, `face_vec`) are the leak, and under LeakSanitizer it exits
  non-zero on that leak alone while doctest reports all 8 assertions passed.
  A re-vendor that copies upstream files over `third_party/sqlite-vec/` drops
  the fix silently; nothing pins the tree, since its sources are tracked here.
- A schema is applied, never invented here: `runSchemaFile` takes the path the
  service resolved, and the DDL of a domain lives in that domain's
  `database/schema.sql` (rule 26). The one exception is the vec0 tables, whose
  shape is the extension's, and they are built by the repository above.
- Opening another domain's database directly is rule 27's exception, not the
  default: it happens through the named client the host installs at boot. The
  two families fail differently, and the difference is load-bearing — the
  identity, camera and productivity clients fall back to the host's own
  database, while `readOnlyClient` does NOT: uninstalled there means the
  tables it serves do not exist at all, and the repositories answer empty
  rather than opening something else in their place.
- `details/` is private by convention — nothing outside this package includes
  `sqlite/details/...`, and a service that needs a vec query asks the
  repository the domain owns, not this one.
- A unit of work's database client is **borrowed, never owned**. The struct a
  caller fills (`ModuleEmitInput`, `ModuleAuditInput`, `CameraUpdateInput`,
  `ChangeOutboxEnqueueInput`, ...) carries a `drogon::orm::DbClient*` that the
  call site resolves with `.get()` from the unit of work's own
  `std::shared_ptr<drogon::orm::Transaction>`; that local is the transaction's
  single owner. The field type is the enforcement: `.client = transaction`
  does not compile, so no struct copy can hold the transaction open. It
  matters because Drogon has no `commit()` — `~TransactionImpl` is what queues
  the commit, and any reference that outlives the commit point keeps the
  caller suspended with no timeout, no error and a leaked connection.
  `db_transaction::Commit(transaction)` moves the caller's `shared_ptr` in and
  resets it, and that reset is what drops the last reference and runs the
  destructor.
- A read may fall back to the pool, a mutation may not: `findById`/`remove`
  take the same non-owning pointer and resolve `client() ? client :
  pooled.get()` themselves, so a caller that names a unit of work gets that
  transaction and a caller that names nothing gets the service's client.
- `freezeClient(dbPath)` is the shutdown half of `client()`, and it exists
  because `app().quit()` resets Drogon's database client manager while
  `DbService::client()` dereferences it — a statement landing in that window is
  a null dereference on the ordinary `SIGTERM` path. A service arms it from a
  `shutdown_signal::onQuit` hook (D23), which runs after the last drain
  reported drained. The flag and the manager access share one reader/writer
  lock (`seamMutex`): `client()` holds it shared across the flag read and the
  `getDbClient()` call while the arming takes it exclusively, so a statement
  that read an unfrozen flag cannot still be inside `getDbClient()` when the
  manager goes. Arming does not borrow, keep or close the app's client: the
  first `client()` after the freeze builds an independent
  `newSqlite3Client("filename=" + dbPath, 1)` that Drogon's manager never
  holds, so it survives the reset with its own loop and connection. It is built
  lazily for that reason — a client created before the freeze has its
  connections closed under it, and a statement arriving after that is buffered
  and never runs. That buffering is the freeze's boundary: a caller that hoisted
  `client()`'s result before the arming holds the app's client across the
  reset, and no lock can follow a pointer already handed out. One freeze per
  process: the first path wins and the rest are ignored.

## Tests

`tests/unit/identity-client-test.cc` — a read-only identity client installed
at boot serves the identity database and refuses a write; and the frozen
client, armed on a booted app's own database and used once that database's own
client reports no available connections — the reset itself, which
`!isRunning()` alone does not prove — is a different client with live
connections that reads and writes the same file. The freeze case comes last in
the file: arming is irreversible for the whole process.
