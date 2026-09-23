# 3a step 2 closure, item 0 — no comments in the code, and a gate that proves it

Scope: the whole first-party tree. The owner decided on 2026-09-23 that the code
carries no comments at all (D21) and put the decision ahead of the nine closure
items as the first step of the continuation. It absorbs closure item 8 (S7, the
statement-level comments of the test suites), whose subject is a subset of it.

## Pre-state, measured

`scripts/check-comments.sh`, written first and run against `HEAD`, reported
**5287 comments in 807 of 1220 scanned files** and exited 1:

| Language | Comments |
|---|---|
| C++ (`.cc` 2120, `.hxx` 1286) | 3406 |
| CMake (`CMakeLists.txt` 739, `.cmake` 101) | 840 |
| TOML config templates and `.env.example` | 235 |
| shell | 200 |
| SQL | 193 |
| protobuf | 187 |
| YAML (compose 116, CI 4) | 120 |
| Python, docstrings included | 59 |
| Dockerfile | 24 |
| `.clang-tidy` | 17 |
| `scripts/lib/tidy-baseline.txt` (the header the scanner writes) | 6 |

Three things in the tree read comment text, found before anything was removed:

- `scripts/setup.sh:38` printed `grep '^#' "$0"` as its `--help`, and
  `scripts/provision-host.sh:36` printed `sed -n '2,18p' "$0"`.
- `scripts/lib/tidy_scan.py:241-246` wrote a six-line comment header into the
  baseline on every `--write-baseline`, so a later re-baseline (closure item 9)
  would have put comments back.
- The schema runners (`db-service.cc`, `schema-runner.cc`, `guard-schema.cc`,
  the three migration tools) skip `--` lines and split statements on a trailing
  `;`; removing comments only removes lines they already skipped. The two
  suites that read `sqlite_master.sql` look for `dead_lettered`, which is in a
  `CHECK`, not in a comment.

Nothing else depended on comment text: no `NOLINT`, no `clang-format off`, no
GCC `fallthrough` comment (`-Wimplicit-fallthrough` would have turned one into a
warning), no `#if 0`, no license header in first-party code, no `//` ending in a
backslash, no here-string in a script.

## What changed

**The gate.** `scripts/lib/comment_scan.py` with `scripts/check-comments.sh` as
its wrapper, in the `check-deps.sh` shape. It lists tracked and
untracked-not-ignored files with `git ls-files`, classifies each by name, and
lexes it with a scanner per language: C-family for C++, protos and JSON (raw
strings with delimiters, char literals, digit separators, line-continued `//`), CMake
(quoted and bracket arguments, bracket comments), shell (single, double and
`$'…'` quotes, `$( )` with `case` patterns, `$(( ))` and `(( ))`, `${ }`,
backticks, heredocs with `<<-` and quoted delimiters, here-strings, a word
boundary tracked by the lexer, the shebang kept), Python (`tokenize` for
comments, `ast` for module/class/function docstrings, the shebang kept, a file
that reads `__doc__` refused), SQL (quoted strings with doubled quotes),
Dockerfile (parser directives such as `# syntax=` kept, heredocs skipped), YAML
(quotes across lines, block scalars with anchors and tags), TOML (basic,
literal and multi-line strings with escapes), `.gitmodules` (`#` and `;`),
`.env.example` (full-line and inline) and hash-line files (ignore lists,
`conanfile.txt`, the tidy baseline). Excluded as vendored or not code:
`third_party/` except its two first-party files (`.gitignore`,
`sqlite-vec/CMakeLists.txt`), the upstream gRPC health proto, `docs/`,
Markdown, TSV, binaries, `LICENSE`, `NOTICE`, `tests/fixtures/` and the
gateway's probe captures. A file type the
scanner cannot classify fails the run (`unclassified:`), a file it cannot lex
fails the run (`unread:`), and a run that checked nothing exits 2. `--fix`
removes every reported span, drops the lines that become empty, and keeps the
layout: no blank line left after an opener or before a closer, no doubled blank
line, no leading or trailing blank lines.

**The wiring.** `scripts/build-all.sh` runs it before `check-deps.sh`, so a
comment stops the run before the Conan install. `scripts/build-all-test.sh`
pins it: the orchestrator names it, the tree passes, a clean fixture full of
look-alikes passes (a URL and `//` inside strings, a raw string holding `--` and
`//`, `1'000'000`, `'"'`, `${#1}`, `$#`, `$(( 16#ff ))`, a here-string holding
`#`, a `^#` regex, a heredoc body, `"#literal"` in CMake/TOML/YAML/Python, a
Dockerfile parser directive, a Markdown file), one comment per language is
rejected with its file and line (ten cases), a docstring is rejected, an
unclassified file type is rejected, an empty tree exits 2, and `--fix` produces
an exact expected file that then passes.

**The strip.** `--fix` over the tree: 5287 comments out of 807 files at first;
after the review, `health.proto` was restored and `sqlite-vec/CMakeLists.txt`
stripped, so the final scanner, run over an export of `HEAD`, counts **5281
comments in 806 files** that are gone, plus `seed-golden.py`, which it refuses
at `HEAD` for reading `__doc__`. Before it
ran, the two `--help` texts became `usage()` heredocs with the same content, and
`tidy_scan.py` stopped writing the baseline header (its reader no longer splits
on `#`). One pre-existing oddity in a touched file was fixed in passing:
`provision-host.sh` had `provision_models() {  local owner script` on one line
at `HEAD`.

**The rules.** `AGENTS.md` rule 20 is now "No comments in code", with the
measured gate and the instruction that every agent and subagent prompt carries
the rule; the gates paragraph names three gates. The old "Minimal comments"
bullet was rewritten in the fifteen unit `AGENTS.md` files that carried it, the
"code, comments, identifiers" lists lost the word, and three statements that
described a comment which no longer exists were rewritten
(`packages/clients/identity/AGENTS.md`, `packages/clients/voice/AGENTS.md`,
`services/sync/CONTEXT.md`). The plan gains D21, §4.10, §4.12 and §4.13 are
rewritten, and item 0 heads the closure table with item 8 marked absorbed.

## Evidence

- **C++, by GCC's own lexer.** All 1002 changed `.cc`/`.hxx` files were run
  through `g++ -x c++ -std=c++20 -fpreprocessed -dD -E -P` at `HEAD` and in the
  working tree; the whitespace-normalised token streams are identical for
  **1002 of 1002**. GCC removes comments itself in that mode, so this is an
  independent check of the scanner, not the scanner checking itself.
- **SQL.** Each `database/schema.sql` was applied to an in-memory SQLite before
  and after: identical `sqlite_master` entries, `table_xinfo`,
  `foreign_key_list`, `index_list` and `index_xinfo`, and identical statement
  text once comments are removed.
- **TOML.** Every template parses with `tomllib` to the same values.
- **Python.** Every file's `ast.dump` is identical once docstrings are dropped.
- **Protobuf.** `protoc -o` descriptor sets are byte-identical for all 21
  first-party files (the vendored health proto is byte-identical to `HEAD`).
- **Compose.** `docker compose config` over `argus-deploy/docker-compose.yml`
  is identical before and after (838 lines).
- **Shell.** `bash -n` passes on all 21 scripts; every deleted line in the
  non-C++ diffs is a comment or a blank line, and every added line is either
  the trimmed form of a deleted one or one of the two `usage()` heredocs.
- **Layout.** No changed file starts with a blank line, ends up empty, gains a
  doubled blank line, or gains a blank line after `{` or before `}`.
- **The gate.** `./scripts/check-comments.sh` → `1227 files checked, 0
  comments`, exit 0; `./scripts/build-all-test.sh` → `build-all tests passed`.
- **Help text.** `setup.sh --help` and `provision-host.sh --help` print the
  text their header comments held; `seed-golden.py --help` is byte-identical
  to `HEAD`'s.

- **The first full gate**, on the stripped tree before the review's fixes:
  `./scripts/build-all.sh dev` exit 0 in 37m40s, **17/17 projects, 401 tests**
  (cert 2, sqlite 2, identity 25, memory 19, intent 4, gateway 30, sync 42,
  camera 50, productivity 34, notification 38, guard 53, tts 20, stt 6, vlm 7,
  llm 32, voice 25, tunnel 12 — the 401 the plan measured before this unit),
  **0 first-party warnings** (the 9 `warning:` lines are ncnn's own TUs,
  recompiled because every project reconfigured), `check-deps` 481 edges and
  0 forbidden, and `check-tidy` **495 TUs, 3140 findings against 3141** —
  identical to the count before the strip, so removing comments moved no
  clang-tidy check.
- **The second full gate**, on the tree as the review left it: the same
  `./scripts/build-all.sh dev`, incremental because only the restored
  `health.proto` and the stripped `third_party/sqlite-vec/CMakeLists.txt` were
  newer than the last build. It ran `check-comments` (`1227 files checked, 0
  comments`), `check-deps` (481 edges, 0 forbidden) and the whole project loop —
  **17/17 projects, 401 tests**, the distribution the first gate saw, and
  **no warnings at all** this time, the first run's 9 being ncnn TUs this one
  did not recompile — and the session ended in the middle of its last stage.
  That stage, `clang-tidy`, was run afterwards on the same unchanged tree
  (`scripts/check-tidy.sh`) and is green: **495 TUs, 3140 findings against the
  3141 baseline**, one check below it, exit 0 — the first gate's numbers, so the
  review's fixes moved no clang-tidy check either. The only file written between
  the two halves is this report: `docs/` holds no translation unit and the
  comment scanner excludes it.

## What this unit does not change

- Generated output that is data, not code, keeps whatever it writes: the
  hardware profile `detect-hardware.sh` writes from a heredoc, the NOTICE the
  VLM provisioning writes, the test fixtures, and `ConfigService`'s runtime
  write path, which still preserves comments an operator puts in their own
  `config.toml`.
- The sibling `frontend/` repository is not part of this tree.
- `docs/history/**` is the record and still quotes the comments that existed.

## The review, and how its findings were applied

A read-only adversarial review (a fresh agent that could not build, because
the gate was running) returned twenty-one findings. It also re-verified the
strip independently: `declare -f` identical for all 19 changed scripts, an
independent CMake tokenizer identical for all 63 changed CMake files, `yq`
identical for every YAML file, a line-splitting emulation of the schema runner
giving an identical `sqlite_master`, zero comments left in the protos by
`protoc --include_source_info`, and the working tree equal to the scanner's
strip of `HEAD` for every file but the five changed by hand. Each finding was
checked against the tree before it was accepted.

| # | Finding | Disposition |
|---|---|---|
| 1 | Every image build fails: the Dockerfiles run `build-all.sh --only`, the build context has no `.git`, and `git ls-files` raised out of `main()` | **Fixed.** `build-all.sh` skips the gate outside a git work tree and says so; the scanner answers such a root with exit 2 and a message, not a traceback. Pinned by two harness cases (a scanner run and a mocked `build-all.sh` run in a tree without `.git`) |
| 2-4 | Shell: a `case` pattern's `)` inside `$( )`, an escaped space before `#`, and `$'…'` quoting could make `--fix` cut code | **Fixed.** Frames track `case`/`esac` per command substitution, the word boundary is the lexer's own state rather than the previous character, and `$'…'` is skipped with its escapes. None of the three shapes is in the tree — `declare -f` is identical — so this is a gate for future code. Pinned in the clean fixture |
| 5, 14, 19 | YAML: a block scalar with an anchor or a tag, a comment after a `- key: \|` block, and a multi-line quoted scalar | **Fixed.** The block pattern accepts `&anchor`/`!tag`, the block's floor is the key's column and its end is the first line less indented than its first content line, and the quote state carries across lines |
| 6 | Dockerfile heredocs (`RUN <<EOF`) were not tracked | **Fixed**, with an unterminated heredoc refused |
| 7 | Line splitting used `str.splitlines`, which also splits on `\f`, `\v` and other separators, and rewrote CRLF | **Fixed**: `\n` only, `\r` kept per line. Measured: no changed file carried any of those characters or a CRLF at `HEAD`, so nothing was damaged |
| 8 | Rule 20 said tool instructions stay while the scanner strips lint suppressions | **Rule reworded**: suppressions are comments and are forbidden (`[[fallthrough]];` for GCC). The three `# noqa: E402` removed from `export-yolo26-ncnn-e2e.py` had no linter reading them |
| 9 | `seed-golden.py` built its `--help` from `__doc__` | **Fixed** with an explicit `DESCRIPTION`; its `--help` output is byte-identical to `HEAD`'s. The scanner now refuses a Python file that reads `__doc__` |
| 10 | `third_party/sqlite-vec/CMakeLists.txt` is first-party and was skipped; `packages/contracts/proto/grpc/health/v1/health.proto` is vendored and was stripped | **Fixed.** The first is scanned (its three comments removed), the second is excluded and restored byte-for-byte from `HEAD`. Flagged, not fixed: that file carried only a provenance line at `HEAD`, not upstream's Apache-2.0 header, and no `NOTICE` names gRPC — a pre-existing gap for the owner of the vendored stubs |
| 11 | 47 of 60 `path:N` citations in live docs pointed at shifted lines | **Fixed.** Every citation of a changed file in the live docs was remapped through a line alignment of `HEAD` against the working tree (39 rewritten); the four that cited a comment were re-read by hand and reworded; two that cite the retired monolith's `database/schema.sql` are history and were left |
| 12 | `deployment-docker.md` said `.env.example` documents every key | **Fixed**: the doc now names the variables the compose file reads beyond the example |
| 13 | Optional keys documented only in template comments were lost | **Fixed** with `docs/operations/configuration-keys.md`, generated from the templates' comments at `HEAD` (every comment line accounted for), linked from the index and from `configuration.md`. The compose file's notes already live in `argus-deploy/CONTEXT.md` |
| 15 | Scripts embedded in other files are not scanned | **Documented** in rule 20 as a review item; none exists in the tree |
| 16 | Valid bash the gate refused (`1 << bits` in arithmetic, `<<END-OF`, a comment inside backticks, `${y:-'}'}`) | **Fixed**, pinned in the clean fixture and, for the backtick comment, as a rejection |
| 17-18 | C++ `--fix` could merge tokens (`-/**/-`), split a `#define` across a multi-line block comment, and trim trailing spaces inside a raw string | **Fixed**: a removed span becomes a space unless whitespace or a bracket is next to it, a multi-line span with code on both sides joins its lines, and only a line whose last span reaches its end is right-trimmed. Measured on `HEAD`: no multi-line raw string shared a line with a comment |
| 19 | TOML `\"` inside `"""`, and `"""doc"""; import os` | **Fixed** |
| 20 | `.gitmodules` `;` comments, inline comments in `.env.example`, JSONC, and stray untracked files of unknown type | The first three **fixed** (a gitconfig scanner, an env scanner, JSON through the C-family lexer). **Ruling:** an untracked file the scanner cannot classify still fails the gate — the tree's rule is that a file the check cannot read is not a file that passed, and an editor's backup belongs in an ignore list |
| 21 | D21's row sat between D19 and D20; closure item 6 still pointed at item 8 | **Fixed** |

Every fixed shape was first run against the scanner as it stood and failed
(eight false positives, five missed comments and a traceback), then passed
after the change; the harness keeps all of them.

