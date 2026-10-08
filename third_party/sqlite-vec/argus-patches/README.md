# argus-patches — local patches to the vendored sqlite-vec

Upstream tree: `sqlite-vec` `v0.1.10-alpha.4` (`sqlite-vec.h`,
`SQLITE_VEC_VERSION`), vendored in-tree at `third_party/sqlite-vec/` and
declared by `third_party/sqlite-vec/CMakeLists.txt`. There is no pin file and
no downloaded archive for this tree: the sources are tracked in this
repository, so a re-vendor is a copy of upstream files over this folder and it
drops every patch here.

| Patch | What it fixes |
|---|---|
| `0001-vec0-free-the-metadata-chunk-shadow-names.patch` | `vec0_free()` never frees `shadowMetadataChunksNames[]`, the metadata chunk shadow names `vec0_init()` allocates for each metadata column (`sqlite-vec.c:5415-5420`). Every `vec0` table creation leaks 2 allocations of 40 bytes for the two tables the argus repositories create (`memory_vec`, `face_vec`): 320 B / 8 allocations, four direct-leak stacks rooted at `vec0_init`, measured under LeakSanitizer by `vec-db-test` in `packages/lib/sqlite`. Upstream documents the obligation in the struct (`sqlite-vec.c:3563-3564`: "The first numMetadataColumns entries must be freed with sqlite3_free()") and frees every other shadow name in `vec0_free()`. The patch adds the free and the NULL, in the same free-then-NULL shape as its neighbours, which is what keeps `vec0_free()` idempotent: `vec0Destroy()`'s `done:` label frees the vtab and its error path leaves it for `vec0Disconnect()`. |

Apply from the repository root:

```
git apply third_party/sqlite-vec/argus-patches/0001-vec0-free-the-metadata-chunk-shadow-names.patch
```

The patch is applied in the working tree as well; `packages/lib/sqlite/AGENTS.md`
records the first-party side of it. A report of this defect for upstream is
drafted and has not been sent anywhere.
