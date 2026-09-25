# Golden HTTP fixtures

One recorded response per route and access shape, replayed against a live
fleet and compared field by field after normalization. The route inventory
comes from the static census (`scripts/lib/route-baseline.txt`, enforced by
`scripts/check-routes.sh`), so a route added without a recording is caught
twice: once by the census gate and once by `verify` reporting it unrecorded.

## Files

| File | Content |
|------|---------|
| `manifest.json` | units, probe count, normalization, invariants, expectations, declared deviations, volatile fields, skipped units |
| `<unit>.json` | the probes of one unit: `unit`, `method`, `path`, `route`, `probe`, `request`, `response` |

`shared.json` holds the `/health` probes. The census attributes that route to
`packages/lib/http`, so it is replayed once per booted base unit
(`plain-auth`, `plain-camera`, …) and skipped for a unit that declares its own
`/health` in the census, which today is guard alone. It is the one bucket that
is not a service.

## Recipe

```bash
scripts/native-stack.sh down
rm -f build/native-stack/*/database/*.db*
scripts/native-stack.sh up
python3 scripts/seed-golden.py --stack-dir build/native-stack
python3 scripts/seed-golden.py --stack-dir build/native-stack --roles
python3 scripts/golden-http.py record --stack-dir build/native-stack
```

A verifying run is the same up to the last line, with `verify` in place of
`record`. Seed in exactly that order: the plain run creates the fixture rows,
`--roles` adds the transient probe users (ids 2 and 3) and mints the three
sessions. A stack seeded in another order gives `GET /user` a different
`created_at` order and the fixture will not match.

`record` writes a new recording; `verify` compares without writing. Both fail
closed when the stack has no fixture row for a slot (`--seed` runs the plain
seeder first), and both clean up after themselves: the transient probe users
and their sessions are removed in a `finally`, so a run leaves the stack's
roster as it found it.

`record` also refuses to run against a stack that is missing a unit it already
has recordings for, or that carries a recording whose unit the census no longer
declares: dropping a recording has to be a deliberate deletion, never a
side-effect of recording on a partial fleet.

## Probes

Every route is probed in each access shape its own filter chain admits:

| Probe | Meaning |
|-------|---------|
| `plain` / `plain-<unit>` | no token, route behind no `JwtFilter` |
| `no-token` | no token, route behind `JwtFilter` — expects 401 |
| `bad-json` | malformed body on a `ValidJsonFilter` route — expects 400 |
| `other-device` | the owner's token with another device's fingerprint — expects 401 |
| `role-guest`, `role-resident` | a session of that role |
| `owner` | the owner's session |
| `owner-missing` | a path id that no row carries |
| `multipart-empty`, `multipart-image` | `multipart/form-data`, empty and with a deterministic PNG |
| `ws-plain-get` | the route's own `WS` method against the WebSocket path, which Drogon answers 405 |

## Invariants

- No probe answers 5xx, except a camera-control route's 502, which must carry
  `CAMERA_UNREACHABLE`.
- A probe with no token answers 401, and so does one whose token belongs to
  another device.
- A malformed JSON body on a `ValidJsonFilter` route answers 400.
- A multipart body is never answered 400.
- A JSON answer carries exactly the `{status, info, errors}` envelope.
- A 204 answer carries an empty body.

## Normalization

`manifest.json` records the rules the recorder applies before writing, and the
same rules apply to the live answer before the comparison, so a value that is
random by nature never becomes part of a contract:

- the keys in `maskedKeys` and any key ending in `At` are masked;
- a value shaped like a three-part JWT, and any 64-character hexadecimal value,
  is masked wherever it appears, inside a JSON body or in a non-JSON one;
- a masked key keeps the shape of what it held, so an object under one is still
  compared field by field;
- the `errors` subtree is not masked by key name: its values are the server's
  own refusal text, which never echoes submitted material, and pinning that
  text is part of the contract.

## Declared deviations and volatile fields

`declaredDeviations` in the manifest exempts a probe from the envelope
invariant, one entry per unit, method and route, each with a written reason.
One entry exists: guard's `/health`, which guard serves from its own handler
and which no other unit's client reads.

`volatileFields` in the manifest lists the fields of a response that are an
observation rather than a contract, and the comparison drops them from both
sides. One entry exists: `/notification/delivery-summary` reports the push
channel's last self-test (`probeOk`, `probeMs`, `latencyMsP50/P95/Max`), which
runs on its own schedule. The counters beside them stay pinned.

## What a run writes

Measured with `scripts/db-snapshot.py`, which digests the row content of every
table of every database under the stack, so a table added later is covered
without editing the instrument:

```bash
python3 scripts/db-snapshot.py --stack-dir build/native-stack --out /tmp/before.json
python3 scripts/golden-http.py verify --stack-dir build/native-stack
python3 scripts/db-snapshot.py --diff /tmp/before.json /tmp/after.json
```

A run is additive on three tables and leaves every domain table byte-identical:

| Table | Effect |
|-------|--------|
| `auth.device_login_challenge` | +1 pending row (`POST /auth/device-login`) |
| `auth.change_outbox` | +1 `sent` row (the logout's user action) |
| `sync.user_action_log` | +1 row (that action's recipient audit) |
| `identity.user`, `auth.refresh_token` | the run's own transient users and sessions; three rows become one at the owner's re-mint, and the last one stays |
| `auth.sqlite_sequence`, `sync.sqlite_sequence` | SQLite's AUTOINCREMENT high-water marks, which gain a row the first time such a table is written |

No fixture row of any domain — person, camera, zone, reminder, project,
notification, invitation — is created, updated or deleted.

## Coverage

What the replay does not pin, so a reader does not mistake the green line for
more than it is:

- the **four units the sandbox does not boot** (`llm`, `stt`, `tts`, `vlm`).
  They are built — `scripts/build-all.sh dev` builds all fifteen projects, so
  each of them has a native binary — but `native-stack.sh` prepares configs
  for the seven request-serving services only, so the harness finds no base
  unit for them (`stack_base` reads `<stack>/<unit>/config.toml`) and prints
  `skip: <unit> is not booted in this stack`. Their ten censused routes carry
  no recording, and the twelve probes the census plans for them (`llm` 3,
  `stt` 2, `tts` 5, `vlm` 2) are the only planned probes the replay never
  sends. All twelve would be `plain` probes — no route of those four carries
  `JwtFilter`, and only `tts`'s two POSTs carry `ValidJsonFilter` — and
  booting four model-loading services to pin them is deferred rather than
  declared covered; the census gate still enforces their filter chains
  statically.
- **multipart success paths**: both multipart routes are recorded only in the
  refusal shapes (422, 401, 409), never with a face that would log in.
- **`PATCH /auth/me` and `POST /guard/mode` success paths**: their probes carry
  a body built to fail validation, on purpose, so a replay cannot rename the
  owner or flip the guard mode.
- **WebSocket frames**: the `WS` probe pins the path and the method rejection;
  the frames themselves are the frozen `/sync` suite's subject.

## Reading a failure

`verify` prints one `drift:` line per failing probe with the field path and
both values. It fails on three other things too: a probe the run has no
recording for (`unrecorded:`), a recording the census no longer has a probe
for (`stale:`), a fixture file whose unit has no route in the census at all,
and a unit that has recordings but is not booted (`unverified:`, counted in the
summary and in the exit code). A failure is a contract change: re-record only
when the new answer is the intended one, and say so in the report.
