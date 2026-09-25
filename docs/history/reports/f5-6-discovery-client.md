# Phase 5 step 6 — a real client against the discovered endpoints

Plan row: `docs/history/plans/architecture-plan.md` "6 | Real client against
the mDNS-discovered endpoints: login, bootstrap, media, voice".

## What the row asks for

The LAN discovery contract has been in the tree since the gateway was
deleted, and until this step nothing had read it off the wire. The row asks
for the client that does, and the sentence has three parts:

1. a client finds the services the way the app will — browse
   `_argus-route._tcp`, read the SRV port, the TXT `path` and the TXT
   `https` — instead of being handed a port number;
2. it completes the four legs against **only** what it resolved: login
   (cross-device), bootstrap (the sync tables), media (the camera's socket)
   and voice (a spoken turn over `/sync`);
3. the announcement is true — the address and port a route announces are
   the ones that route answers on, and the sandbox's app-facing services
   announce every route the route baseline declares for them, not a subset
   of them.

Part three is what makes the exercise worth more than a smoke test: a
responder can be wrong in a way that only a browser on the LAN sees, and
every unit suite in the tree reads `health()` instead.

## 1. The spine, as the code states it

**The contract.** `packages/contracts/routes/src/routes/service-discovery.hxx`
declares the spellings: service type `_argus-route._tcp`, TXT keys `path`
and `https`. `packages/contracts/routes/AGENTS.md` states the rule that
gives the type its shape — **one instance per route, not per host**: a
service with five routes announces five instances under the same type, the
instance label is `<mdns.name with dots folded to dashes>-<path segment>`,
the SRV record carries host *and* port (so two instances may name the same
port), and TLS is announced in TXT rather than assumed from the port.

**Who builds them.** `packages/lib/http`'s `routeAnnouncements()` walks the
routes a service registered and builds one `MdnsInstance` per logical
route; each app-facing service hands that list to `MdnsService` in its
`main.cc`, along with the port its own listener bound. A client therefore
learns everything it needs from the records: the host from the A/AAAA of
the SRV target, the port from SRV, the path segment and the scheme from TXT.

**Why no existing suite could have caught what this step found.** The mdns
suite reads `MdnsService::health()` — the responder's own JSON — and
`route-discovery-test` asserts the record *values* the contract produces.
Both are computed from the same in-memory data the wire is built from; a
defect in how that data is *encoded into the packet*, or in which records
the responder chooses to answer with, is invisible to both.

**The four legs, as the code states them.** Login is the cross-device flow
(`services/auth/src/feature/auth/`): `POST /auth/device-login` mints a
256-bit challenge with a 120 s life, `GET /auth/device-login/{id}` polls
`pending`/`approved`/`expired` and is single-use, `POST
/auth/device-login/{id}/approve` is the paired device's own session, and
`PATCH /auth/refresh-token` rotates single-use. Bootstrap is
`SynchronizedService` over `/sync`: the socket pushes
`SocketEmitDto{operation:0, option:"user"}` on connect, the `sync` message
answers with **one** frame — `operation:1`,
`option:"user"`, and an `info` that holds `{created, deleted, lastSyncDate}`
per table, fifteen of them, whichever subset the request named — and
`sync_audit_log {findLast:true}` answers `operation:2` with a monotonic
`watermarkId`. `services/sync`'s message vocabulary is `sync`,
`sync_audit_log`, `sync_user_audit_log` and the `camera:`/`voice:` prefixes;
anything else is refused `400` as `{type:"<type>_error", status, error}`.
Media is the camera's `/media` socket:
`camera:subscribe{cameraId,quality}` → `camera:ready{mime:"video/mp4"}` →
`camera:closed{reason}`. Voice is `VoiceGrpcRelay` over the same `/sync`
socket: `voice:start`, binary PCM frames back and forth, `voice:stt`,
`voice:assistant`, `voice:stop` → `voice:done`.

## 2. The instrument

`scripts/discovery-drill.py`, verbs `resolve`, `login`, `bootstrap`,
`media`, `voice`, `all` and `state`. It speaks DNS itself — an encoder, a
parser that follows compression pointers behind a jump guard, TXT parsing,
and a query with the **unicast-response bit** set (`CLASS_IN | 0x8000`) sent
from an ephemeral port to `224.0.0.251:5353`. That bit is not a detail: a
plain class-IN query from an ephemeral port is answered **multicast to
5353**, which an ephemeral client never sees, so a browse written the
obvious way measures nothing and reports an empty LAN.

The client dials `endpoint(route)` — announced address, announced port,
announced path, announced scheme — and `all` reuses the one discovery for
all four legs. What keeps the claim honest is that every announcement is
cross-checked against the thing it claims to describe:

- the announced port equals the port the unit's own `config.toml` binds;
- the announced address is one of this host's own addresses;
- TXT `path` equals the route segment and TXT `https` equals whether that
  unit's listener terminates TLS;
- the sandbox's seven app-facing services announce every route
  `scripts/lib/route-baseline.txt` declares for them: the expected set per
  unit is that file, so a route that exists in the code but never reaches
  the LAN is a failure with its name in it. The baseline's 75 rows cover
  twelve units, and the four AI services' ten of them are internal — the
  seven `main.cc` sites that call `routeAnnouncements()` are all app-facing;
- every announced instance resolves — it carries an SRV port and a TXT path,
  so an advertisement with a missing record set is a failure rather than a
  silently absent route;
- every instance's label is the contract's composition, asserted record by
  record: the host prefix the unit's own `config.toml` declares (or the
  responder's `kDefaultName`, read from the source rather than copied), the
  route segment, the service type and exactly one `.local`. A `/camera`
  wearing `Argus-zone._argus-route._tcp.local` fails this check, which is
  why it is the one the negative control below trips;
- no two routes are announced under one record name, and no two instances
  claim one route path;
- a browse of a service type nobody serves (`_argus-nothing._tcp`) answers
  nothing — the negative control that stops "found seven instances" from
  being satisfied by noise;
- a **single packet carrying two questions** is answered for both, which is
  the shape the responder's shared buffer got wrong (§4);
- with nothing advertised at all the drill refuses to run, naming the
  sandboxes whose `config.toml` has `mdns.enabled = false` and the
  `ARGUS_STACK_MDNS=1` boot that fixes it.

Every socket the client hangs up is followed by `/health` plus a pid
comparison, which is step 5's regression guard reused here: the client in
this drill closes a sync socket, a media socket and a voice socket, which
is exactly the shape that killed `argus-sync` before that fix.

The drill re-mints the recorder's session (`seed-golden.py --roles`) before
the pairing leg, so the login leg measures the login and not the freshness
of the seed: the seeded refresh token is single-use, and without this a
second run would report the sandbox's spent session as a login failure.

## 3. Measured

Two consecutive `all` runs on a sandbox booted advertising
(`ARGUS_STACK_MDNS=1`, seven services healthy on 7044, 7042, 7026, 7027,
7028, 7025 and 7039): **110 checks, 110 passed, 0 failed** — the summary
prints a `skipped` clause only when something was skipped, and nothing was —
the same check set both times, and the same three pids before and after. The
numbers quoted below are the first run's; where the two differ it is the
seconds that move (the greeting 6.7 s → 6.3 s, the transcript 10.3 s →
9.6 s), never a check. Two further runs on a sandbox booted from the binaries
of the full `build-all.sh dev` below — the build that carries every fix in
this report, the catalogued refusal included, and the second run after the
`subId` nit of §5 was tightened — report the same **110 checks, 110 passed,
0 failed**, exit 0, with the voice leg's greeting at 6.6 s and its transcript
at 9.8 s and the same pids on either side of the sockets.

**The announcement, browsed off the wire.** 17 routes answer under 17 distinct
record names — `Argus-<route>._argus-route._tcp.local` — all 17 resolving to
an SRV port and a TXT path, all carrying `192.168.18.205`, every label the
composition the contract declares for that route (prefix `Argus`, its own
segment, one `.local`), and every route announced under a name of its own.
Each route's announced port equals the port its own unit bound, its TXT
`path` equals the route segment, its TXT `https` equals whether that listener
terminates TLS (true for all seven), and each unit announces exactly the
routes `scripts/lib/route-baseline.txt` declares:

| Unit | Routes announced |
|---|---|
| identity | `invitation`, `pairing`, `portrait-preview`, `user` |
| camera | `camera`, `media`, `zone` |
| productivity | `calendar-event`, `calendar-event-share`, `project`, `project-member`, `project-task` |
| notification | `notification`, `notification-token` |
| auth | `auth` |
| sync | `sync` |
| guard | `guard` |

Negative control: `_argus-nothing._tcp.local` answers nothing. Two questions
in one packet (`_argus-route._tcp.local` and `_services._dns-sd._udp.local`)
are both answered — the check that fails against the pre-fix responder (§4.2).

**Login, on the discovered `https://192.168.18.205:7042`.** `POST
/auth/device-login` answers 200 with a 64-hex `challengeId` living 120 s; the
new device polls `pending`; the paired device approves it over its own
binding; the new device collects `approved` with `userId 1`, `role owner`,
`name "Golden Recorder"`; a second collection reads `expired`; the session it
just minted answers on the discovered endpoint (200 in **6 ms**); rotation
answers 200 with a pair in which both tokens differ from the ones presented;
and the rotated-away refresh token is refused **401** on the replay. The last
two were the checks the token collision made impossible (§4.3).

**Bootstrap, on the discovered `https://192.168.18.205:7025/sync`.** The
socket greets with `{operation: 0, option: "user", info: {id: 1, isActive:
true, role: "owner"}}`; the `sync` frame answers **one** frame carrying all
four requested tables in **13 ms** with the seeded rows (3 `user`, 1
`camera`) and each table's `{created, deleted, lastSyncDate}` — a list, a
list and an object, asserted as such; the `sync_audit_log {findLast: true}`
sent on the **same** socket answers `operation 2` with `watermarkId=1`.

**Media, on the discovered `https://192.168.18.205:7026/media`.**
`camera:subscribe{cameraId: 1, quality: "main"}` answers `camera:ready
{mime: "video/mp4", subId: 3}` in 0.0 s, followed by
`camera:closed{reason: "upstream_failed"}` — the seed's camera has no
credentials, so the failure is named rather than a stream faked. The `subId`
is the camera process's own subscription counter (`nextSubId_ = 1` in
`stream-hub.hxx`), so the sandbox these runs used — subscribed twice before
them — answered `3`, while a fresh boot answers `1` and the drill after it
`2`; the check requires the mime and an integer `subId` that is not a `bool`,
not any particular number.

**Voice, on the discovered `/sync`.** `voice:start` answers the greeting
"Hi Golden I'm Argus, your assistant. How can I help you?" after 6.7 s with
one binary PCM frame beside it; the client sends 106000 samples of `0.wav` in
34 binary frames; `voice:stt` returns the transcript ("after early nightfall
the yellow lamps would light up here and there the squalid quarter of the
brothels.") in 10.3 s; `voice:assistant` answers "That sounds like a vivid
scene."; `voice:stop` closes the turn with `voice:done`. The whole AI tier is
in play here (`argus-llm`, `argus-stt`, `argus-tts`, `argus-voice`), which is
what makes the leg worth running rather than skipping.

**The sockets the client hung up.** Three pid comparisons, before the first
socket and after the last: auth `1926312 → 1926312` (`/health` 200 in 9 ms),
sync `1926563 → 1926563` (200 in 10 ms), camera `1926378 → 1926378` (200 in
16 ms).

**Negative control.** A check that cannot fail is the defect §5 is about, so
one was reproduced live: with `guard`'s sandbox config declaring
`mdns.name = "ArgusTampered"` while its responder was still announcing the
`Argus` it booted with, `resolve` reports **88 checks, 87 passed, 1 failed**,
exit 1, and the one failure is the check that owns the property —
`/guard is announced under the contract's label`, whose detail reads
`'Argus-guard._argus-route._tcp.local' for host 'ArgusTampered' and route
'guard'`. Restoring the config returns 88/88/0.

## 4. The defects the client found

### 4.1 Two responder defects, both on the wire

Both were invisible to the suites above, and both fixed in
`packages/lib/mdns/src/mdns/mdns-service.cc` with a unit regression case
that fails against the pre-fix code.

**A doubled `local.` in every instance name.** `normalizeServiceType()`
recognised the suffix in one spelling only — it compared the string's last
six characters against `.local` — while it always terminates its own output
with a dot (`if (type.back() != '.') type.push_back('.'); type += "local."`),
so the function was not idempotent. It is called twice per instance:
`buildRecords()` normalizes the caller's type and hands the result to
`instanceNameFor()`, which normalizes again. The first pass therefore answers
`_argus-route._tcp.local.`, and on that string the suffix test reads `local.`
where it looks for `.local`: nothing is stripped and a second suffix is
appended. Simulated against the pre-fix source, all four spellings the
callers can pass (`_argus-route._tcp`, with a trailing dot, with `.local`,
with `.local.`) end the first pass dot-terminated and the second pass at
`_argus-route._tcp.local.local.`; post-fix every one of them is idempotent.
Every browser on the LAN read:

```
Argus-auth._argus-route._tcp.local.local
Argus-camera._argus-route._tcp.local.local
```

`health()` reported `_argus-route._tcp.local.` correctly, which is why the
unit suite — which asks `health()` — was green throughout. The fix strips
trailing dots before the suffix test, so the function is idempotent for
every spelling of the type; the new case
(`every spelling of a service type keeps one local suffix`) drives all four
spellings — `_argus-route._tcp`, with a trailing dot, with `.local`, with
`.local.` — and asserts the composed instance label for each, and reverting
the fix makes it fail with exactly
`Argus-camera._argus-route._tcp.local.local.` The label is the honest
witness: `health()`'s `serviceType` field echoes the caller's input verbatim
(it iterates the requested list, not the advertised records), so asserting it
would have measured nothing — which the four-spelling form showed when it
failed on that line for the three un-normalized spellings.

**Only the first route of each service was answered.** `handleQuestion()`
walked the advertised instances and `return`ed on the first whose service
type matched, so a PTR query for `_argus-route._tcp` got one record per
service instead of one per route. Measured on the running sandbox before
the fix — a browse of this host saw **7 routes of 17**:

| Announced (pre-fix) | Hidden |
|---|---|
| `auth`, `sync`, `camera`, `guard`, `invitation`, `notification`, `calendar-event` | `media`, `zone`, `pairing`, `portrait-preview`, `user`, `calendar-event-share`, `project`, `project-member`, `project-task`, `notification-token` |

An app that resolved `camera` and dialled `/media` on it would have got a
404; `/portrait-preview` and `/pairing` — the two routes identity exists to
serve a new device — were unreachable by discovery entirely. The loop now
answers every matching instance and tracks whether it answered anything, so
the branch below it (instance names) keeps its behaviour.

### 4.2 The answer overwrote the query it was still parsing

The responder read the query into one 2048-byte packet buffer and then
**wrote the answer into that same buffer**
(`mdns_query_answer_unicast(sock, …, buffer.get(), bufferCapacity, …)`). The
library's listener holds a pointer *into* that buffer and walks the packet's
questions with it (`mdns_socket_listen` recomputes `MDNS_POINTER_DIFF(data,
buffer)` per question and re-reads the name through `mdns_string_equal`), so
answering question 1 replaced the bytes question 2 was about to be parsed
from. A query packet carrying two questions got **one** answer: the second
question was parsed out of an answer packet and dropped. The library ships
`mdns_multiquery_send` for the query side of exactly this shape, so a real
resolver may batch, and nothing on the wire said so.

The drill measures it: one packet asking `_argus-route._tcp.local` and
`_services._dns-sd._udp.local` must be answered for both. Pre-fix, the meta
question — which the responder does answer, and at length, one PTR per
service type — came back empty because the service-type answer had already
landed in the buffer.

The fix gives the responder a second buffer, `answerBuffer`, used only by the
answer path and only from the responder thread (`announce()` runs before it
starts and `goodbye()` after it joins, so the receive buffer is that thread's
alone).

### 4.3 Two sessions minted in the same second were the same token

The login leg failed with `401 Device mismatch` on a token it had *just*
collected, and the rotation was refused with `Invalid or expired refresh
token`. The cause was in `JwtService::generate()`: a token carried
`{iss, sub, iat, exp}` and nothing else, so two sessions minted for one user
inside the same second were **byte-identical**. Measured in the sandbox's
`refresh_token` table during the run that failed — the ids are that run's,
because the seeder mints three sessions and the drill re-mints the recorder's
before every pairing leg, and `device_hash` is the first 16 hex of the 64
the column holds:

```
id  user_id  device_hash       user_agent                  is_valid is_used
10  1        d1fbcc866cc9815e  argus-golden-recorder/1.0   1        0
11  1        4c22abeb0cf78ea6  argus-discovery-drill/1.0   1        0
```

Both rows carried the same access-token signature and the same refresh-token
signature; decoding their payloads gives the same `iat` (`1790375325`) and
two different `device_hash` values — the paired recorder's and the device
pairing in. Row ids are a property of the run, not of the defect: the same
two sessions read out under other ids after a reseed.

`RefreshTokenRepository::findByAccessToken` matches on
`(user_id, access_token, is_valid, is_used)` and returns the first row. With
two rows holding the same token, the verdict for the new device was read off
the recorder's row, so the device hash disagreed; the refresh rotation looked
the token up the same way and failed the user-agent comparison, which the
server logs as `Auth: user agent mismatch on refresh for user 1`.

This is a product defect, not an instrument one, and the tree already knew
about it: `services/auth/tests/unit/session-verdict-test.cc` mints its
fixtures with a hand-rolled incrementing `jti`, working around the collision
in the test instead of the service. The fix adds a random 128-bit `jti` in
`JwtService::generate()`, so every minted token is unique by construction;
a caller that supplies its own `jti` still overrides it, and
`packages/lib/auth/tests/unit/jwt-service-test.cc` pins both halves — 500
mints for one subject, all distinct, each carrying a 32-hex identifier, and a
caller's own identifier surviving. Dropping the `.set_id(randomTokenId())`
line fails it with the colliding payload (`{"exp":…,"iat":…,"iss":"argus",
"sub":"1"}`, no `jti`) as the counter-example. The same table reads out the
post-fix shape: two rows for `user_id 1` with one `iat` (`1790376729`) and
two 199-character tokens that differ, each carrying its own `jti`
(`5e1e2bda…`, `9ea175b9…`).

The lookups themselves are right — `findByAccessToken` and
`findByRefreshToken` match on the token value, and `schema.sql` carries plain
indices on both columns. Making them **unique** would turn the next
collision into a loud insert failure, and it is deliberately not done here:
the statement would refuse to build on any database that already holds the
duplicates this bug wrote (the sandbox's above, and a developer's), so it
belongs with the dedup that migrates them, in a change of its own. Uniqueness
by construction is what the fix delivers.

### 4.4 The app's own client browses a service type nobody announces

The drill is the tree's second client. The first is the app, and it is still
looking for the gateway that was deleted:

| Where | Says | The fleet announces |
|---|---|---|
| `frontend/src-tauri/src/net/discover.rs:7` | `_argus._tcp.local.` | `_argus-route._tcp`, one instance per logical route |
| the same resolver's answer | one `Discovery{host, ip, port, https: true}` | one record set per route, with TXT `path` and TXT `https` |
| `frontend/src/shared/constants/net.constant.ts:3` | `ARGUS_DEFAULT_PORT = 7024` | no listener on 7024 since the gateway went |

Every announce site in this tree reaches the responder through `lib/http`'s
`routeAnnouncements()` (`route-announcements.cc:21`), so
`routes::kServiceType` is the **only** service type any first-party
service is ever given — `_argus._tcp` is announced by nothing. A phone on
the same LAN therefore resolves nothing, and the resolver's hardcoded
`https: true` and `host: "argus.local"` are the per-host, TLS-assumed model
the route contract replaced (TLS is announced in TXT, never assumed). The two
TS constants beside the browse are declared and read by nothing: only
`ARGUS_HOST` is imported, by `use-pairing-flow.ts:5`.

This is the frontend's unit rather than this one — the app needs a
route-aware resolver (which route does it dial for auth, for sync, for
pairing?), its `Discovery` shape and its pairing flow revised, and its own
gates; a Tauri build is not something this repo's gates can verify. It is
recorded here because step 6 is the step that would otherwise have claimed
the client works against the discovered endpoints, and because the two
halves of this contract now disagree on the wire: the backend's half is
measured true (§2), the app's half is measured absent.

## 5. Defects found while building the instrument

An adversarial read of the drill (905 lines then, 1010 now) came back with
eleven findings; every one was re-measured against the tree and the wire
before being acted on, and all eleven were real. They are listed because the
shape recurs: a check that prints `ok` for a property it never measured is
worse than no check, and this step is entirely made of checks.

**The three socket legs could not upgrade at all.** `announced_routes()`
keyed the table by the TXT path normalized, but stored `path` verbatim, and
`SyncSocket` interpolates that string straight into `GET {path} HTTP/1.1`.
The contract's TXT `path` is a **bare** leading segment (`sync`, `media`), so
the client asked for `GET sync` and Drogon answered `404 Not Found` — the
route exists at `/sync`. Measured both ways against the live listener:
`GET sync` → 404, `GET /sync` → 401 (the 401 being the route genuinely
existing). Every leg returned before its first assertion. The table now
carries `request_path` (rooted) beside the verbatim `path` (asserted), and
the dial line prints what it dials.

**The bootstrap spoke a message type the server refuses.**
`BOOTSTRAP_REQUEST` said `"type": "synchronize"`; `SyncService` accepts
`sync` (`sync-service.cc:74`), and the frozen fixture
`services/sync/tests/fixtures/sync/sync-bootstrap.json` records the same
request. The refusal (`{"type":"synchronize_error","status":400,…}`) carries
no `operation` key, which is exactly what the drain loop tested for, so the
server's own words were dropped and the leg then reported an empty answer.

**…and modelled the reply backwards.** `sync` answers **one** frame —
`operation:1`, `option:"user"`, `info` holding the fifteen tables — so
collecting "one frame per table keyed by `option`" could never be satisfied
by a correct server, and `sorted(answers) == sorted(BOOTSTRAP_TABLES)` was
false by construction. The drain now takes the frame's `info` as the table
map, requires each requested table inside it, and checks the per-table
`{created, deleted, lastSyncDate}` shape against it.

**The sync-contract check sent nothing.** `WATERMARK_REQUEST` was defined
and never used, so the leg collected for a `sync_audit_log` answer nobody had
asked for: the watermark — the half of the contract that pages a reconnecting
client — was unmeasured, and the check could not pass.

**The replay predicate accepted "no answer" as a refusal.**
`status != 200` is satisfied by `0`, and `http()` maps a `SystemExit` (an
unreachable endpoint, `Connection refused`, a TLS failure) to exactly that.
An `argus-auth` that was down for the instant of the replay printed `ok the
rotated-away token is refused`, and so did a 5xx. The predicate is now
`status in (401, 403)`.

**A pid comparison that was vacuous when `pgrep` matched nothing.**
`service_pids()` returns `[]` silently when the pattern misses — a different
`--profile`, or any deployment not launched from
`services/<unit>/build/<profile>/` — and `[] == []` passed while three
"is the same process after the client hung up" checks printed `ok`. Both
sides being non-empty is now part of the check.

**A literal `True` check.** "The resolver received `_argus-route._tcp`
answers" asserted the constant, adding one free passing check per run; the
property that actually guards it was a `SystemExit` above, which exits before
the summary and prints no named failure. It is replaced by a real one: every
PTR-announced instance must resolve to an SRV port and a TXT path.

**`advertising_off()` read the wrong key.** Its regex was section-blind and
matched only the spaced spelling, so `auth`'s `[rate_limit] enabled = false`
and `guard`'s `[guard] enabled = false` made the "nothing is advertised" hint
name units whose mDNS is on. It now reads `[mdns] enabled` through the
section-aware config reader, which gained bare-TOML-boolean support for it.

**The collision detector and the resolver disagreed on what a path is.**
The resolver keyed by `path.lstrip("/")` while `duplicate_paths()` grouped
the raw string, so an instance announcing `path=/sync` beside one announcing
`path=sync` collapsed into a single table entry (the later silently winning)
while "no two instances claim one route path" passed. Both now normalize.

**Two assertions were weaker than the sentence printed beside them.** The
challenge check accepted a 128-bit id (`len >= 32`) where the server mints 64
hex, and a five-minute life (`<= 300`) where `kDeviceLoginTtlSeconds` is 120.
Both tightened to what the server does.

**A skip that the arithmetic did not show.** The spoken half of the voice leg
is skipped when `argus-llm` is absent — `llm_running()` is a `docker ps`
probe, so a natively run AI tier takes the skip path too — and the summary
still read "N checks, N passed, 0 failed". Skips are now counted and named in
the summary (`… , 1 skipped` plus a `SKIPPED:` line — one is the most the two
mutually exclusive skip branches allow, and with everything up no `skipped`
clause is printed at all). The probe itself stays container-only; a native AI
tier is not detected, and the summary says it was skipped rather than
implying it passed.

Verified and deliberately left alone: the DNS encoder/parser and the
unicast-response bit (a plain class-IN query from an ephemeral port is
answered multicast to 5353 and an ephemeral client never sees it — the bit is
load-bearing); the route expectation, derived from
`scripts/lib/route-baseline.txt` rather than from the drill's own output; the
media and voice expectations against `camera-media-service.cc`,
`stream-hub.cc` and `voice-grpc-relay.cc`; and `SyncSocket` itself, which
verifies the server's `Sec-WebSocket-Accept`, masks its frames and returns
`None` on a timeout or a close rather than turning a wire failure into a
pass.

### The second read: the report and the C++ diffs

Two more adversarial reads — one over this report and the instrument, one
over the three C++ files — came back with findings that were re-measured the
same way. Acted on:

- **the label check asserted less than §2 claimed**: it compared the prefix
  and the type but never the route segment, so permuting the 17 labels passed
  it, and the fleet check beside it was `{'Argus'} ⊆ {'Argus'}` in a sandbox
  where all seven units share one name. Both are now the one exact comparison
  against the contract's composition, per route, which is what the negative
  control above trips;
- **the bootstrap shape check claimed `created` and asserted two other keys**;
  it now requires `created` and `deleted` to be lists and `lastSyncDate` an
  object, which is what the frozen fixture carries;
- **the rotation check asserted only that some access token came back**; it
  now requires both returned tokens to differ from the pair presented;
- **two quotes the instrument cannot print**: no run prints `0 skipped`, and
  one skip is the reachable maximum (§3, §5);
- **`randomTokenId()` refused with a bare `std::runtime_error`**, a
  per-request failure outside rule 6's vocabulary. It throws the catalog's
  `TokenIssuanceFailed` now, beside the two issuance failures it resembles,
  and the pinned-catalog test carries the entry (24 → 25) so the refusal is
  inside the table rather than outside it;
- **nothing pinned the `jti` the fix exists for** — deleting the line failed
  no test in the tree. `packages/lib/auth/tests/unit/jwt-service-test.cc`
  pins 500 distinct mints and the caller's override, and fails without
  `.set_id(randomTokenId())` (§4.3);
- **nits**: a `watermarkId` of `true` no longer satisfies the contract check,
  nor a `subId` of `true` the media one, and the responder's default name is
  read from `kDefaultName` rather than copied into the drill.

Measured and deliberately left alone:

- **a second `initialize()` on a live `MdnsService`** reassigns the two
  packet buffers under the responder thread and assigns over a joinable
  `std::thread` — which terminates at the assignment, before any buffer is
  freed. Every caller in the tree calls it once (the seven `main.cc`
  registrations and each unit case), so the guard belongs to an API-contract
  change of its own, not to a fix that only added a second allocation to the
  same path;
- **the unique index on `refresh_token`'s token columns**, for the migration
  reason in §4.3;
- **symptom-level coverage of the doubled `local.`** (a case that reads the
  advertised records rather than `health()`): the records are not a public
  surface of `MdnsService`, and the idempotent normalizer is the root fix the
  existing case pins.

## 6. What the drill deliberately does not assert

- **The frontend.** The app's own resolution, cache and paint-first
  behaviour is the app's suite; the backend's contribution is that the
  announcement is true and the legs answer off it. What the app asks for
  today is measured in §4.4.
- **LAN delivery.** The announcement is multicast without a checked result
  (`packages/lib/mdns/AGENTS.md` says so), so the drill browses the wire but
  cannot prove the packet left the host — it proves what a browser *on this
  host* reads, and the client dials the address in that answer rather than
  the loopback.
- **TLS trust.** The client verifies no certificate, as every other drill in
  this tree does; the certificate's SAN is `packages/lib/cert`'s subject.
- **What a real camera streams.** The seed's camera has empty credentials,
  so `/media` answers `camera:closed{reason:"upstream_failed"}` — the drill
  asserts the subscription, the mime type and that the failure is named,
  not that frames arrive.
- **A second host.** Discovery is measured on one machine; a real laptop on
  the same subnet is the same records one hop away, and nothing here can
  prove the router forwards multicast.

## Gates

`check-comments` 1420 files / 0 comments, `check-deps` 135 declarations /
911 edges / 0 forbidden / 0 cycles / 0 unresolved, `check-routes` 75
declarations over 12 units / 56 behind `JwtFilter` / 2 multipart / 0 added /
0 removed / 0 forbidden, and `build-all.sh dev` exit 0 — 15 projects, 467
registered ctest cases, 0 failed, not one `warning:` line — with `check-tidy`
at 554 TUs / 2875 findings over 45 checks against the 2901 baseline, 9 checks
below it and none risen: the new test file is the 554th translation unit and
it added no finding.

## How to run it

```bash
ARGUS_STACK_MDNS=1 flock -o /tmp/argus-build.lock ./scripts/native-stack.sh up
python3 scripts/seed-golden.py --stack-dir build/native-stack

python3 scripts/discovery-drill.py all      # every leg
python3 scripts/discovery-drill.py state    # what is advertised, without driving
```

The `all` verb needs `argus-llm` up for the spoken half of the voice leg; it
prints a `skip` line and still measures the greeting when it is not.

## Files

| File | Role |
|---|---|
| `scripts/discovery-drill.py` | the client: the DNS browse, the resolver, the four legs, the announcement cross-checks |
| `packages/lib/mdns/src/mdns/mdns-service.cc` | the two responder fixes (`normalizeServiceType`, `handleQuestion`, the answer buffer) |
| `packages/lib/mdns/tests/unit/mdns-service-test.cc` | the regression case that fails against the pre-fix normalizer |
| `packages/lib/auth/src/auth/jwt-service.cc` | the random 128-bit `jti` that makes two minted sessions distinct, and the catalogued refusal when entropy fails |
| `packages/lib/auth/tests/unit/jwt-service-test.cc` | the case that fails without the `jti`: 500 distinct mints, a caller's override |
| `packages/contracts/auth/src/auth/auth-errors.hxx` | `TokenIssuanceFailed`, the refusal the mint throws |
| `packages/contracts/auth/tests/unit/auth-contract-catalog-test.cc` | the pinned catalog, 24 → 25 entries |
| `scripts/native-stack.sh` | `ARGUS_STACK_MDNS=1`: the sandbox boots advertising |
| `scripts/isolation-drill.py` | the socket client the drill reuses, now able to dial a path other than `/sync` |
| `scripts/lib/route-baseline.txt` | the route set the fleet's announcement is checked against |
