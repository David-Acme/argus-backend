# Kùzu crash reproducer — upstream v0.11.3 and the Vela fork

Phase 0 gate of COGNITIVE_MEMORY_PLAN.md, with a corrected harness. Argus is
a CPU-first home assistant; the probe inserts household-scale facts into
Kùzu (384-d embeddings + vector/FTS indexes) and interleaves writes with
reads.

## Harness corrections (2026-08-11)

The first probe had two defects, now fixed:

1. The recall loop ran at 100 % duty cycle (no think time between turns),
   starving the worker on glibc's non-FIFO `std::mutex`. The reader now
   idles `--turn-ms` (default 2000) between turns, matching Argus' real
   ~0.5 % duty cycle (one recall per 2–5 s conversation turn).
2. Batched mode issued `BEGIN TRANSACTION` under a lock that was released
   before the batch finished, leaving a write transaction open on the shared
   connection while the reader ran inside it. `BEGIN..COMMIT` now live under
   a single lock acquisition.

**The crash below reproduces with both defects fixed**, in the cleanest
possible harness: single thread, one Database, one Connection, auto-commit
per statement, insert→read interleaved. It is a defect of the engine, not of
the harness.

## Reproducer A — upstream v0.11.3: single-thread, single-connection crash

```bash
git submodule update --init third_party/kuzu       # kuzudb/kuzu @ v0.11.3
cmake --preset dev
cmake --build --preset dev -j 8 --target argus-kuzu-probe
cd build/dev/labs/kuzu-probe
./argus-kuzu-probe --facts 100 --seq-load --seq-query mix
```

Sequence: insert ~1 300 rows (100 facts + 200 entities + 600 rels + 300
aliases) with auto-commit, build vector/FTS indexes, then interleave
`CREATE (:Fact {…})` with `QUERY_VECTOR_INDEX` / `QUERY_FTS_INDEX` /
2-hop `MATCH` on one thread. After ~5–50 interleaved cycles (once the WAL
crosses the checkpoint threshold) it fails non-deterministically with:

```
stl_vector.h:1253: ... Assertion '__n < this->size()' failed.
[kuzu::storage::DirectedCSRIndex in LocalRelTable::getCSRIndex paths]
```

or hangs permanently, immune to SIGTERM. A `--seq-query match` loop (plain
scan, no index) and `--no-load` (all writes, then all reads) always pass —
the failure needs the write→read cycle with index queries.

## Reproducer B — Vela fork v0.12.0-vela: any second connection

```bash
git submodule update --init third_party/kuzu-vela   # v0.12.0-vela.87bf0be
# repoint labs/kuzu-probe/kuzu-db.cc's Database/Connection to the fork build
```

Any significant activity through a second `Connection` (read or write) on
the same `Database` asserts identically in `DirectedCSRIndex`, even with no
concurrent reader.

## Environment

- Debian x86_64, GCC 16 (libstdc++), CMake 3.25+, C++20
- Upstream commit: `v0.11.3` (27cba5b91423c96a0a0507c92dfe0e1654f7f184)
- Fork commit: `v0.12.0-vela.87bf0be` (87bf0bef9c39a762775f66ad675070fed1e516fc)
- Local build patches on both (modern GCC): thrift `<cstdint>`,
  `KUZU_BINARY_DIR` in `extension/{fts,vector}/CMakeLists.txt`,
  directory-scoped `-include cstdint`. `fts` + `vector` statically linked
  and auto-loaded at `Database` construction.

## Numbers (Release, @ 5 000 facts, idle store)

| Metric | Value |
|---|---|
| RSS delta at open | 54 MB |
| KNN p95 (HNSW, cosine, 384-d) | 13 ms |
| 2-hop traversal p95 | 2 ms |
| BM25 FTS p95 | 46 ms |
| Single insert (idle) | ~12 ms |
| Interleaved insert+read (single thread, auto-commit) | crash / SIGTERM-proof hang past the WAL checkpoint threshold |
