# CONTEXT.md — why this folder exists

## Origin

The Argus backend (C++20/Drogon monolith) is being migrated to microservices;
`src/` disappears entirely over that migration and every repo-root folder
becomes a service or a package. This folder was `packages/identity` (step
f7-2d) and is the identity service's home; Phase 3c step 1 turned it into a
service — a process with its own boot, listeners and database, instead of a
module the gateway linked — and the gate's project list swapped the package for
it, leaving the count at eighteen until Phase 3d step 1c deleted the gateway
and took it to seventeen.

What the package was: everything `src/identity/CMakeLists.txt` compiled as
`argus_identity` beyond the cross-domain modules — the invitation, pairing and
user features (the `auth` one became `enrollment` in Phase 3b-2, when the
session surface moved to `argus-auth`), the identity repositories and schemas,
faces and the private-portrait storage — moved here unchanged, relative paths
preserved. Step 1 then moved each of those files a second time, from
`src/feature/api/<resource>/` to `src/feature/<resource>/` and from
`src/feature/rpc/` to `src/app/rpc/`, which is rule 23's shape for a service.

## The standalone process

The extraction is a process, not a folder: `src/app/main.cc` loads
`config.toml`, opens `database/identity.db`, applies
`database/schema.sql` from its beginning advice, installs the four filters,
registers the four controllers, connects the NATS bus and installs the change
sink, builds the gRPC server on `[server] grpc_port` (7040), and runs Drogon on
`[identity] port` (7044) with the instance certificate.

Before step 1 the gateway HOSTED both surfaces: it constructed the RPC service
and bound `identity.rpc_host:rpc_port`, and it proxied the HTTP route. That was
the strangler-pattern state the plan calls "contract now, binary later" — the
gRPC contract was designed for the standalone shape from the start, so the
extraction changed CMake, boot and deploy, not file locations. The gateway's
own `src/identity/identity-config.*` and `src/identity/identity-registrar.*`
died with it: the gateway kept the public API on 7024 and reached this service
through `identity.proxy_url` (HTTP) and `identity.target` (gRPC), and read the
notifiable roster through `argus::clients::identity` instead of the tables it
used to query. Phase 3d step 1c deleted the gateway and `identity.proxy_url`
with it, so this service's own listener is the public API now.

## Pairing and the first owner

`POST /pairing` accepts the random code in `pairing.code` (see
`packages/lib/cert`), under a process-wide lock so two callers cannot both
pass the check-then-set, and records the paired device's hash
(`DeviceFilter` now runs on the route) as `[pairing] owner_device`. The
first registration of the installation becomes the Owner only from that
same device: auth forwards the caller's device hash in
`RegisterUserRequest.device_hash` and enrollment answers `NotPaired`
otherwise. Before, the code was derivable from the CA certificate served in
every TLS handshake, and anyone who registered first after the pairing -
the legitimate owner or not - became the Owner. An installation paired
before this change has no `owner_device` and keeps working.

**The code never travels.** The client pairs over a trust-any TLS
connection (it has no CA yet), so a code sent in clear would hand the secret
to whoever sits in the middle, who could then answer with its own CA. The
client sends a random `nonce` (32-64 hex) and
`proof = HMAC-SHA256(code, "argus-pair-client|" + nonce)`; the server checks
it in constant time and answers, with the CA, `serverProof =
HMAC-SHA256(code, "argus-pair-server|" + nonce + "|" + caFingerprint)`. The
client accepts the CA only when that proof matches the fingerprint of the
CA it received, which an intermediary cannot forge without the code. The
plain `{code}` body is still accepted for scripts and older clients. The
code is upper-case hex, so both sides use it upper-cased as the HMAC key.

The code is checked first, then the paired state. Pairing only hands out
the CA a device needs to trust the server; it opens no session (that takes a
face login or an approval from a signed-in device). So every device that
proves the code is paired, which is how a second phone or the desktop app
joins after the first pairing, and only the first pairing records
`owner_device`, the one device the first Owner may register from. It used to
answer 409 to every device after the first, which left the desktop QR login
- a flow that starts from a paired desktop - with no way in.
`GET /pairing/status`
(`DeviceFilter` only) answers `{paired, hasOwner}`, which is how a signed-out
app decides between owner enrolment and login; it replaces the gateway's old
`/auth/has-admin`, which no service has answered since the gateway was
deleted. It shares the `/pairing` prefix, so the remote gate keeps it on the
LAN.

## The port the pairing and invitation answers publish

`PairingController` and `InvitationFeatureService::resolve` both answer the
port a client dials next, and both read it from
`IdentityConfig::resolveAnnouncedPort()`, which is this service's own listener
port. Until Phase 3d step 1c that read was `mdns.port` — the installation's
public port, 7024, while the gateway advertised and served it — and the key is
gone from every config. The app pairs against that port, stores it as the
paired instance's port and puts it in every later invitation QR, and it rejects
an answer whose port is not positive or does not match the port mDNS
advertised. Phase 3d step 1b made identity an advertiser in its own right: it
announces one `_argus-route._tcp` instance per logical route it registers
(`invitation`, `pairing`, `portrait-preview`, `user`) at its own listener port,
so the port the answer publishes is the one the instance the app paired through
carries.

## What moved and what didn't

Moved: 107 source files (the four features' `.cc` + headers, the ten
`src/shared/repositories/` triplets, the seven `src/shared/schemas/` pairs,
`src/shared/services/face/{face-service,face-db}.{hxx,cc}`,
`src/shared/services/storage/private-portrait-service.{hxx,cc}`),
`database/schema.sql` (the DDL truth for every identity table), the identity
unit suites and the migration tool (now `tools/migrate-identity/`, where the
root `tools/` dissolution landed it).

The copy names the source's own columns and the verification checksums those
columns on both sides, refusing only a source column the target lacks
(2026-10). It used to copy with `SELECT *` and compare the column lists of
`src` and `main` read as `"src".pragma_table_info(...)`, which SQLite
resolves against the main schema: the check compared the target with itself,
and a legacy table that predates a later additive column failed the copy on
its column count. `tests/unit/identity-migration-test.cc` pins both cases.

Not moved, on purpose:

- The auth filter package (`src/filter/`) — `argus-auth` landed as its own
  service in Phase 3b-1 and Phase 3b-2 moved the `/auth` surface, the three
  session tables and the rate limiter onto it; what stays here is the
  `enrollment` feature that answers `RegisterUser`.
- The audit / sqlite / cert / socket / mdns / room modules — cross-domain or
  gateway-owned; they dissolve into their owner services later. `socket`,
  `room` and `audit` did, into `services/sync` in Phase 3a-1c; `sqlite` and
  `cert` are the `lib` packages this service still links. `mdns` is not one of
  them either: it is the `lib/mdns` package this service reaches through
  `lib/http`, which announces one `_argus-route._tcp` instance per logical
  route at this service's listener — the port its pairing and
  invitation answers publish (below).
- `src/auth/identity-change-sink.hxx` and the other sink contracts — consumed
  through the module links' include roots. The contracts now live in
  `argus::contracts::sync` and the identity sink is this service's own
  (`src/feature/user/services/nats-identity-change-sink.{hxx,cc}`).
- `database/identity.db` — live data. The service opens it from `database/` by
  default (`[identity] db`), and the schema path default is
  `services/identity/database/schema.sql`; the deploy binds the schema into the
  container.

## The include-prefix invariant

Every moved file kept its `feature/...` / `shared/...` relative path, so the
include root stayed `src/`, exactly as the package's was
`packages/identity/src`; the only include edits the move made are the retired
`api/` segment's removal in thirteen files (`<feature/api/x/...>` →
`<feature/x/...>` — eleven feature sources and the two unit tests), and the
same segment dropped by the header that became
`src/app/rpc/identity-rpc-service.hxx` in its rename. Each include of a file
that stayed
in `src/` was audited to resolve through a declared module edge —
`argus::lib::cert`, `argus::lib::sqlite` (db-service, vec-db),
`argus::lib::auth` (jwt-service, the filters), `argus::lib::config`,
`argus::lib::validation`, `argus::lib::runtime` (the cancellation and threading
wrappers), `argus::lib::storage`, `argus::contracts::sync`,
`argus::contracts::voice` (the `VoiceLang` a new user's language is carried in)
and `argus::contracts::identity` (the generated stubs the RPC services
implement). Two edges of that audit are gone since Phase 3a-1c: `argus::socket`
and `argus::audit` were deleted with their packages, the sink contract moved
into `argus::contracts::sync` and the sink that implements it into this
service's own feature tree, so identity now publishes onto the feed instead of
writing the audit tables.

## The auth⇄identity cycle is gone (f7-3)

`argus::lib::auth`'s filters used to read this service's repositories (device
credential by secret hash; user and refresh-token by the JWT chain), which made
the dependency mutual — this service's controllers declare those same filters.
f7-3 deleted the reading half: the filters called `argus.identity.v1`
(ValidateToken, CheckDeviceCredential) through `argus::clients::identity`
instead. Phase 3b-3 moved that call a second time, onto the verdict argus-auth
serves (`argus.auth.v1`, through `argus::clients::auth`), so nothing in the
filter path names this service at all. What remains is one direction only —
the identity service → `argus::lib::auth`, the filter declarations its own
controllers carry — and no consumer's link order matters anymore.

## The RPC surface

`src/app/rpc/identity-rpc-service.cc` serves `argus.identity.v1`:
UpdateUser (the F6-3 spoken-name write), RegisterUser (the `enrollment`
feature), GetUser and the person/face calls (ListPersons, IdentifyPerson,
EnrollPerson, TouchPerson, TagPerson, ListNotifiableUsers, GetPersonTags,
GetPerson, PromotePerson). `IdentitySyncRpcService` serves the same contract's
`SyncService.PullTable` leg for `user`, `user_invitation` and `person` — the
rows `services/sync` pages when a client bootstraps, alongside the change feed
this service publishes.

The gRPC server raises its receive limit to 12 MiB: the auth DTOs accept a
10 MB face image for login and registration, and gRPC's default 4 MiB limit
answered a full-resolution phone photo with `RESOURCE_EXHAUSTED`, which the
caller reported as an unrecognized face.

The listener starts before `FaceService::init()` runs, so the inference slots
start closed and a call that arrives during boot waits for the models.
Either way the slots open: `init()` opens them whether the models loaded or
not, and `disable()` opens them when `[face] enabled` is false. A call then
answers `nullopt` instead of parking a thread forever, which is what the
camera's matcher used to hit on every crop while recognition was off. Once
recognition is known to be off, a call answers before taking a slot or
decoding the image; during boot it still waits, so a login that races the
model load is not refused as an unrecognized face.

A face image is refused before it is decoded when its header declares more
than 12000 px on a side or 50 MP in total: OpenCV decodes a PNG at full size
before scaling it, so a ~1 MB 20000x20000 PNG posted to the unauthenticated
`/auth/login` allocated over a gigabyte and could get identity OOM-killed.

`IdentifyPerson` sets `face_found` (an optional field added to the frozen
contract, so older callers ignore it): whether the detector found a face
in the crop at all. Without it a crop of someone's back and an unmatched
face were the same answer, and guard counted both as strangers.

PromotePerson answers `UNAVAILABLE` when argus-auth gives no verdict and
`UNAUTHENTICATED` only for a verdict that says invalid, the same split
`JwtFilter` makes.

The f7-3 pair ValidateToken and CheckDeviceCredential left with the session
surface in Phase 3b-1/3b-2: argus-auth serves them now, and it is the single
authoritative validation — JWT signature, the live user row (status always
fresh, no cached verdicts) and, when the caller's device filter ran, the
refresh-token row with its expiry and device binding. It answers OK with
`valid=false` and the caller's 401 body rather than a gRPC error, so a rejected
token and a broken service stay distinguishable — an unreachable authority
makes the filter fail closed. `IdentityRpcService` holds `filterAuthClient()`
for the one thing that still needs a live verdict here: PromotePerson's Owner
actor check.

## The unit suites

`identity-config-test` (the config module's defaults, its section overrides and
the non-loopback RPC listener's secret gate — the cases the gateway's suite
carried while it hosted this surface), `identity-migration-test` (schema apply
+ the argus.db → identity.db row by row verification),
`identity-change-outbox-test` (the outbox's own key rules and dispositions),
`identity-change-transaction-test` (a feature write and its outbox row commit
together), `identity-change-outbox-sink-test` (every leg of the sink, plus the
live round trip when `ARGUS_NATS_URL` names a broker) and
`identity-sync-rpc-test` (the `SyncService` pull leg: the role gate, the
rule-7b user scope, the tombstones and both sides of the fleet-secret gate)
register in this service's standalone CTest graph. `device-credential-test`
left with the credential flow it drives (Phase 3b-2), where it is
`services/auth`'s `device-login-test`. No e2e suite exists yet; the folder
gains `tests/e2e/` when the service has one.

## The change feed's durable outbox (3a-2e)

This feed publishes on two subjects — the change subject the memory catalog
replicas follow and the action subject the journal subscriber reads — where
camera, notification and productivity each publish on one. The module
therefore carries a `subject` column: a row states where it goes instead of the
drain inferring it from a payload that does not always name its own kind (a
journal row has no `kind` field at all).

The two legs are addressed differently. A change leg names a *transition*: its
`event_id` is the hash of the table, the record and the payload, so a
redelivered transition is a replay and the same record cannot be published
twice for one move. A journal leg names an *action*: a portrait view changes no
row, so two views of one portrait carry byte-identical payloads and a
content-derived id would collapse them into one audit row — the kind of loss
nobody would notice. A journal row therefore carries a **minted** `event_id`,
`identity-action:` followed by 32 lowercase hex digits drawn from
`RAND_bytes` at enqueue, and travels under that id as its `Nats-Msg-Id`. The id
must be opaque rather than positional: once Phase 3c-2 split the journal into
`sync.db` its row ids come from a table the identity outbox does not own, so an
id derived from them would collide with whatever the sibling owner's own rows
happen to be numbered. Settlement stays a status-guarded CAS over the row id
(the minted id guards the broker's duplicate window, not the flush), and rows
enqueued before the minting still flush under the row-derived
`identity-action:<id>` they were published with.

The sink registers with `shutdown_signal` at this service's boot — it used to
register with the gateway's, which hosted the package — so a SIGTERM stops the
drain before Drogon's `quit()` destroys the database client manager the worker
reaches through `DbService::client()`. The drain's worker is a thread of the
sink's own, so nothing in Drogon stops it, and the hook waits for it to report
drained rather than quitting underneath it. The registration comes before
`drogon::app().run()` — the hook's handlers are what `run()` installs Drogon's
own `sigaction` over, and a signal that arrived before the first registration
would run Drogon's default quit with no drain wait — and therefore before the
`reconcile()` that starts the worker, because a drain registered after the stop
was requested is only stopped at once, never waited for.

The drain wakes at the commit. `enqueue` runs inside the feature's
transaction, so the row is invisible until it commits; the sink's
`db_transaction::CommitObserver` wakes the drain when a commit lands and its
`WakeSignal` keeps a wake that arrives mid-pass. A wake before the commit
used to find nothing and leave the change waiting the 500 ms retry period.

`identity.db` used to be the one database two owners wrote — this service and
`services/sync`, which applied its five tables into the same file between Phase
3c-1 and 3c-2. Both were free to call their table `change_outbox`, and
`CREATE TABLE IF NOT EXISTS` would have silently let whoever booted second
adopt the first one's shape; Phase 3c-2 removed the question by giving the sync
owner a file of its own, and this service's outbox is the only `change_outbox`
left here.

## Telling argus-sync about a role change

After a role change or a deactivation commits, identity asks argus-sync to
move the user's sockets between role rooms, emits `AuthContextChanged` and,
for a deactivation, disconnects them. Those are synchronous gRPC calls with
a 5 s deadline, so they run in a `BlockingTask`, never on the request's IO
loop, where an unreachable argus-sync would stall every request that loop
serves. Each one takes a ticket right after its commit and waits for its turn
(`SessionNoticeOrder`): two quick changes A→B then B→C must reach argus-sync
in that order, or the socket ends up in both B's and C's rooms while the
database says C.

## Accounts the owner turns off (2026-10)

Deactivation is `PATCH /user/{id}` with `isActive:false` (or `DELETE
/user/{id}`), reactivation the same patch with `true`. Two refusals guard it:
the last active Owner can be neither deactivated nor demoted (409
`ActiveOwnerRequired`, as before), and nobody deactivates their own account
(409 `SelfDeactivationForbidden`), refused before the transaction opens so
nothing is written or queued. The owner turning a user off is the whole
flow: the commit publishes the user row, argus-sync closes the user's
sockets, and argus-auth's consumer revokes every session; reactivation
restores the ability to log in, not the sessions. `IdentifyPerson` now says
`account_disabled` when a face belongs to a disabled user instead of answering
as if the face were unknown, and `RegisterUser` answers
`REGISTER_USER_ACCOUNT_DISABLED` for a disabled user's face, so argus-auth can
tell that person why (403 `ACCOUNT_DISABLED`). The camera guard keeps seeing
no user id for that face, as before.

## Invitations are single use, with a server-side lifetime (2026-10)

The owner no longer chooses a capacity or an expiry: `POST /invitation` takes
the role alone, and every invitation is created with `max_redemptions = 1`
and `expires_at = now + [invitation] lifetime_seconds` (1800 s by default,
clamped to 60..86400). The columns stay, so the synced metadata, the redemption
CAS (`TRY_CONSUME`) and the CHECK constraints are unchanged and the app reads
`expiresAt` as before.

Why one use and no input: the QR is shown once and only for as long as the
owner keeps it open, and closing or unmounting it revokes the invitation. A
capacity above one turned a photographed QR into an open door for several
people, and an expiry chosen in days kept a token valid long after the person
it was meant for had joined.

Why a lifetime at all: the revoke-on-close is a client action, and it can be
lost (the app is killed, the phone loses the network, the request times out)
while the token is still valid. OWASP's guidance for one-time tokens
(Forgot Password Cheat Sheet: tokens must be random, long enough, stored
securely, single use and "expire after an appropriate period"; ASVS 4.0 2.3.1:
activation codes "expire after a short period of time") is that single use is
not enough on its own. Thirty minutes covers installing the app, pairing and
the face enrolment while the owner holds the QR open, and bounds what a lost
revoke leaves behind; the app shows the QR as expired when it lapses instead
of a date the owner never chose. The token itself keeps rule 7b: 256 bits of
`RAND_bytes`, only its SHA-256 stored, never logged or returned after
creation, consumed in the same transaction as the enrolment.

## Camera guard surface (camera-guard phase 2)

`IdentifyPerson`, `EnrollPerson`, `TouchPerson`, `TagPerson` and
`ListNotifiableUsers` are fleet-secret gated RPCs. Enrollment creates a person
without user (empty name), persists the embedding and its `face_vec` row,
optionally stores the JPEG crop in `person_snapshot`, and emits the `person`
sync add. `person_tag` holds LLM tags. The face engine stays inside this
process; argus-camera only ships crops.

**Unknown faces expire.** Every distinct stranger the cameras crop becomes a
candidate person with an embedding and, optionally, the JPEG crop — in a
restaurant or an office that is every customer's biometrics, and the exact
vec0 search grows with it. `CandidateRetentionService` (feature `retention`)
retires, every six hours, the candidates nobody promoted and no user is linked
to whose `last_seen_at` is older than `[retention] candidate_days` (default 30,
`0` keeps them): one statement soft-deletes up to 200 rows and returns them,
the same transaction deletes their embeddings, crop and tags and publishes
the sync tombstone (`{id, deletedAt}`), and their `face_vec` rows go once it
commits. A stranger who comes back after the window is a new candidate; a
known person and a user's person are never touched. Because the index rows go
after the commit, a stop in between would leave rows pointing at a retired
person, which a later sighting would match; `FaceDB::init` therefore drops
every `face_vec` row whose `face_embedding` row no longer exists.

`PromotePerson` (candidate → known) is the one mutating call with a human gate:
the owner bearer token plus the device fingerprint travel in the call, the
service verifies the access token, requires an Owner actor with an active bound
session on that device (`hasActiveSession`), promotes, and publishes the
`person` module audit. `person.status` (`candidate`/`known`) defaults to
`known` for legacy rows; `IdentifyPerson` reports
`trusted = (status == known)` and attaches the linked user only for active
users.

The schema used to carry `notification_delivery_inbox` and the three audit
tables — the gateway's durable delivery receipts and its audit trail. Phase
3a-1c moved all four into `services/sync/database/schema.sql`, whose owner
applies and writes them; this file is identity's tables and nothing else.

## The file split (Phase 3c-2)

This service and `services/sync` wrote one SQLite file until Phase 3c-2 split
the five sync tables into argus-sync's own `sync.db`. `argus-migrate-sync`
copies them (row-count and checksum verification over exactly the keys the run
copied; the deploy runs it as `--profile sync-init` forward and `--profile
sync-rollback` with the paths swapped), and
the journal's redelivery key was re-minted at the same time so
`user_action_log.msg_id` no longer borrows a row id from a table this owner
writes. Nothing here owns, renames or drops those five tables any more.

## Face embeddings are stored as BLOBs

Drogon's SQLite binder sends a `std::string` parameter as TEXT with length -1,
so SQLite reads it up to the first zero byte. The two writers of
`face_embedding` (the enrollment feature and `FaceEmbeddingRepository::create`)
passed the float bytes that way, and every canonical row was cut short —
usually to nothing, since an embedding is full of zero bytes. Search never
noticed because `face_vec` is written through the vec connection with a real
blob bind; the loss would have surfaced the day an index was rebuilt from the
canonical rows. Both writers now bind a `std::vector<char>` (Drogon's BLOB
type), and `identity-face-embedding-test` pins that a row with zero bytes comes
back whole and typed `blob`. Rows written before the fix stay truncated; the
`face_vec` index still holds their real vectors.

## Voiceprints (speaker identification, learned passively)

Argus learns the voice of each account holder by itself, from the holder's own
calls, and recognizes it afterwards (1:N). Nobody enrolls: the owner decided
(2026-10-03) that this is an internal capability of a local system and must not
be presented to the user, because every byte stays on the user's own computer.
The feature is `src/feature/voiceprint/`, and it mirrors the face design: the
engine runs in this process, the canonical rows store only embeddings, and a
vec0 table (`voice_vec`) is the search index.

### Why here, and why sherpa-onnx

Biometrics stay in identity (rule 1 of this service's AGENTS.md). Extracting
in argus-stt and storing here would ship every enrollment's raw audio across a
process boundary, make each verification depend on a second service, and split
one biometric across two owners. sherpa-onnx is already vendored
(`third_party/sherpa-onnx`) and built the same way by argus-stt; this project
builds its C API (`sherpa-onnx-c-api`) against the Conan onnxruntime, so the
cost is build time, not a new dependency. Its speaker extractor carries the
Kaldi-compatible fbank front end and per-model normalisation (read from the
ONNX metadata), which a hand-written front end would have to reproduce exactly.

### The model, measured

Candidates came from sherpa-onnx's `speaker-recongition-models` release. They
were scored offline on two corpora: Mini LibriSpeech `dev-clean-2` (16 English
speakers, 141 target / 2 115 impostor trials per length) and 26 Chilean
Spanish speakers (OpenSLR 71, 182 / 4 550 trials). Enrollment is the centroid
of three 3-second clips; the test clip is 2 or 3 seconds. EER (%), lower is
better:

| Model | Size | Dim | EN 2 s | EN 3 s | ES 2 s | ES 3 s | ms per 3 s (2 threads) |
|---|---|---|---|---|---|---|---|
| 3D-Speaker ERes2Net VoxCeleb (**chosen**) | 26 MB | 192 | 2.08 | 1.53 | 1.54 | **0.08** | 100 |
| 3D-Speaker CAM++ zh-en advanced | 28 MB | 192 | 0.73 | 0.66 | 2.63 | 1.08 | 48 |
| 3D-Speaker ERes2NetV2 zh-cn | 71 MB | 192 | 2.13 | 1.55 | 1.66 | 0.05 | 266 |
| WeSpeaker ResNet34-LM VoxCeleb | 26 MB | 256 | 7.83 | 6.74 | — | — | 101 |
| 3D-Speaker CAM++ VoxCeleb | 30 MB | 512 | 26.7 | 15.5 | — | — | 37 |

The households Argus serves speak Spanish first, so the model that is best in
Spanish at the lengths a phrase or a call turn actually has wins: ERes2Net
(VoxCeleb2 dev, 5 994 speakers; 0.83 % EER on VoxCeleb1-O upstream), Apache
License 2.0 (ModelScope `iic/speech_eres2net_sv_en_voxceleb_16k`). The two
512-dim/256-dim exports behaved badly through sherpa's front end and were
dropped. `services/identity/scripts/provision.sh` downloads the file into
`models/speaker/` with a pinned SHA-256 (`.part` + atomic move) and writes the
NOTICE beside it.

The operating points come from the same runs. A 1:1 score of 0.50 on
3-second clips gives a false-accept rate of 0.36 % (EN) / 0.50 % (ES) at a
false-reject rate of 2.3 % / 0 %; 2-second clips 0.28 % / 0.31 % at 5.7 % /
3.3 %; the passive gates use that 0.50 as "sounds like this person". 1:N
(`identify_threshold` 0.55, `identify_margin` 0.05 over the runner-up) is
stricter because a search over N people multiplies the false-accept rate by N.

Every speaker-model run goes to the blocking pool's heavy lane
(`BlockingLane::Heavy`, `packages/lib/runtime`): `SpeakerEmbeddingService::analyzeAsync`,
the 1:N search in `VoiceprintFeatureService::identify` (analysis and nearest
neighbours in one task) and the turn analysis of
`PassiveEnrollmentService::learnFromTurn`. The ERes2Net forward pass costs
about 100 ms per 3 s of audio, so a call's turns would otherwise hold light
slots that the session verdicts, vec0 lookups and repository work need. The
close-of-call scoring (`scoresFor` over the centroid) is a vector search, not
a model run, and stays light. The face paths were already there:
`FaceService::identifyAsync` and `extractImageAsync` run heavy, the vec0
search and index writes after them light.

### What is stored

Three tables, all of them embeddings or counters; raw audio never reaches a
table, a file, a log or the change feed (it is decoded in memory, measured,
embedded and dropped):

- `voice_profile` (one row per user, `UNIQUE user_id`): the model id, the
  L2-normalised centroid (float32 BLOB), how many call samples stand behind it,
  their voiced seconds, the `source` (`passive`, or `enrolled` for a profile
  carried over from the explicit enrollment this replaced), `linked_at` and
  `refreshed_at`.
- `voice_sample`: one row per **call** that taught something — the centroid of
  that call's accepted turns, its turn count and voiced seconds, the device
  hash it came from and its `state`: `pending` (heard before the voice is
  linked, or too weak to refresh it) or `adopted` (stands behind the profile).
  At most `maxPendingSamples` (20) pending rows per user, kept 30 days
  (`voiceprint.window_days`), and `maxProfileSamples` (40) adopted rows, kept
  180 days; older rows go at every call of that user and in an hourly purge.
- `voice_device`: per `(device_hash, user_id)` the calls heard, how many matched
  the linked voice, how many conflicted with it and how many showed two
  voices. It is the device prior below.

The `voice_vec` index is rebuilt from the profiles of the active model at every
boot, so a stop between a commit and its index write repairs itself.
**Versioning:** a profile or sample whose model id differs from the configured
model is ignored and replaced by learning on the new model; two embedding
spaces are never compared.

The explicit enrollment of 5773a063/c477b761 (`voiceprint`,
`voiceprint_challenge`, `voiceprint_challenge_sample`) is gone. At boot
`VoiceProfileRepository::migrateLegacy()` copies any `voiceprint` row into
`voice_profile` as `source = enrolled` (that person consented and was
confirmed) and drops the three tables; the step is idempotent.

### How a voice is learned

The voice relay (argus-voice) sends every call turn of at least 2 s to
`VoiceprintService.ObserveTurn` together with the account the call belongs to
(`user_id`), the device hash argus-sync's `JwtFilter` bound to that socket and
an opaque per-call key, and closes the call with `CloseCall` when it hangs up
(an idle sweep closes calls silent for 5 minutes). The answer is the same
`IdentifyVoiceResponse` `Identify` gives — the call's hint is never delayed —
and the learning runs after the reply, off the request path
(`PassiveEnrollmentService`).

Every gate is a pure function in `services/passive/passive-policy.{hxx,cc}`
(unit-tested with synthetic voices) and the per-call state lives in memory in
`VoiceCallTracker` (bounded: 64 open calls, 12 turns each, the last 256 closed
keys remembered so a late turn cannot reopen a call).

**Per turn** (the turn is skipped, the call keeps going):
- quality: at least 2.0 s voiced, 15 dB SNR, at most 0.5 % clipped samples —
  stricter than identification (0.8 s, 12 dB, 1 %).

**Per turn** (the turn *taints* the call; a tainted call teaches nothing):
- *two voices in one turn*: the voiced span is cut in halves (each ≥ 0.9 s),
  each half is embedded, and halves scoring below 0.15 against each other mark
  a mixed turn. Measured on the LibriSpeech fixtures with ~1.2 s halves: one
  speaker 0.24–0.68, two speakers 0.06;
- *another enrolled person*: a turn scoring ≥ 0.50 against someone else's
  profile and at least as high as against the holder's own is never adopted,
  and the call stops teaching;
- *drift*: a turn below 0.55 against the centroid of the call's accepted turns
  (one speaker scores ~0.77 on these fixtures, different speakers ≤ 0.0);
- a turn that arrives under the same call key from another account or device.

**Per call**, at close: not tainted, at least 2 accepted turns and 5 s of
speech, and every turn ≥ 0.55 against the centroid of the others
(leave-one-out). The call's centroid is then compared with every profile: if it
sounds like someone else (≥ 0.50 and at least as close as to the holder),
nothing is stored.

**Linking** (`evaluateLink`, over the user's samples of the last 30 days, both
states) clusters the call centroids (seeded at the call with most neighbours ≥
0.60, refined three passes) and links only when all hold, in this order:
1. *dominance*: the cluster holds ≥ 75 % of the window's calls
   (`voiceprint.link_dominance`) — two people taking turns on one account stay
   at ~50 % and link nobody; a guest or a TV-only call now and then does not
   outvote the holder;
2. *occasions*: ≥ 3 calls (`voiceprint.link_min_occasions`) **from the user's
   own devices**, counted as occasions an hour or more apart (a dropped call
   resumed, or a guest's back-to-back calls, are one occasion);
3. *days*: those occasions span ≥ 2 local calendar days
   (`voiceprint.link_min_days`) — one evening of a visitor on the owner's phone
   is never enough;
4. ≥ 20 s of speech in the cluster;
5. the cluster is not ≥ 0.50 like any other person's profile.

*Own device* is the device prior: a device hash on which only this account has
called. A tablet two accounts use (household members sharing a device) never
counts toward a link for anyone; its calls still refresh a linked voice when
they match it strongly (margin +0.05).

**Refresh**: once linked, a call whose centroid scores ≥ 0.60 against the
profile (0.65 from a shared device), that does not sound more like someone
else, and whose device does not mostly conflict with the profile (> 50 % of
≥ 4 known calls) is `adopted`. Every 3 adopted calls the profile is
recomputed: the robust mean of the newest 40 adopted samples — samples further
than max(3 σ (MAD), 0.15) below the median similarity, or under 0.45, are left
out — must agree with the current profile (≥ 0.60) and is blended 30 % into it,
so the voice drifts with the person (a cold, a new phone) but never jumps.

**Relink**: weak or foreign calls stay `pending`; if a different voice ever
fills the window enough to pass every link gate (dominance counts the adopted
samples too), the profile is replaced and its old adopted samples dropped —
this is how a wrong link heals once the real holder keeps calling.

Every link, relink, refresh and forget publishes one `UserAction` journal event
on `TableName::User` through the identity change sink, inside the same
transaction, with safe metadata only (`event`, `source`, `automatic`,
`occasions`, `days`, `samples`, `outliers`, `model`, `byOwner`, never a vector
or a score). Nothing about voices enters the sync stream.

### What a voice match may do

- It is a **soft hint**, never authentication: there is no voice login, and no
  consumer may unlock, arm, disarm or authorize anything on a voice match. The
  call keeps the session's role, tool user and memory owner whatever the voice
  says (`services/voice/CONTEXT.md`).
- There is no anti-spoofing model (the open ones are trained on ASVspoof
  logical-access attacks and are unreliable on phone-microphone replay), so a
  recording can match; that is acceptable only because a match unlocks
  nothing.
- Learning never needs a person's action, so nobody can steer it from the app:
  it only reads calls the account holder opened with their own session on a
  bound device.
- The biometric stays on the host: the RPCs answer scores and ids, never
  vectors, and request bodies are not logged.

### Residual risks, by design

- A person who uses somebody else's account on that account's own phone for
  most of its calls, on several days, will be learned as that account's voice.
  That is the account's real user for every practical purpose; the hint stays
  a hint.
- A TV or radio dominating a call while the holder never speaks yields a
  consistent call of the broadcast voice; it is outvoted by dominance unless
  it happens in three of four calls.
- Learning needs the person's own consent since 2026-10-04 (see "Privacy
  choices" below): without it nothing is learned or matched, and withdrawing
  erases what was learned. The owner can still *forget* a voice, and Argus
  learns it again from later calls only while the person consents.

### Surfaces

HTTP (TLS 7044), Owner only (`RoleFilter`: `/voiceprint/...` maps to no table
in `role-access`):
- `GET /voiceprint/users` → `{available, recognized: [{userId, since,
  updatedAt}]}` — whose voice Argus recognizes, for the people directory.
- `DELETE /voiceprint/user/{id}` → 204: forget the profile, every learning
  sample and the device prior of that person (404 `VoiceprintNotFound` when
  nothing was learned, 404 `UserNotFound` for an unknown user).

gRPC `argus.identity.v1.VoiceprintService` (fleet-secret gated, client
`VoiceprintClient` in `argus::clients::identity`): `Identify` (guard and
anyone who only asks who is speaking), `ObserveTurn` and `CloseCall` (argus-voice).
`CreateChallenge`, `Enroll`, `Verify`, `Delete` and `GetStatus` left with the
explicit enrollment; their outcome values are `reserved` in the proto.

### Tests

`identity-voiceprint-audio-test` (no model): WAV decoding, raw PCM,
resampling, the speech-quality analysis (including the stricter clipping
limit) and the vector maths. `identity-voiceprint-passive-test` (no model, no
database): every gate of `PassivePolicy` and `VoiceCallTracker` on synthetic
192-dim voices — mixed turn, other speaker, drift, call cap, tainted, too few
turns, too little speech, inconsistent call, dominance (a 50/50 household
links nobody, 3 of 4 links the holder), occasions, local days, shared device,
speech total, voice taken, adoption (weak, shared margin, other better, device
conflicted), refresh (outlier left out, gradual blend, a stranger's reservoir
refused) and the tracker's bounds. `identity-voiceprint-test` (live Drogon
loop and database, the real model, the LibriSpeech fixtures turned into calls
by cropping, gain and noise): the legacy carry-over, the gates' calibration,
Rita linked after three calls on two days from her phone (not after two), the
journal event and its safe metadata, a TV voice in a call (drift → tainted), a
mixed turn, Rita's linked voice on Gil's account (other speaker), Gil's
account shared 50/50 with another voice (nothing linked), a tablet two
accounts use (nothing linked), three later calls adopted and refreshing in one
batch, the gRPC `ObserveTurn`/`CloseCall` path with the fleet secret, and the
owner's forget (404 after). Without the model on disk the model-backed half
reports itself skipped.

## The face pipeline fed the recognizer garbage (2026-10, STRANGERS)

Measured before any change, through `FaceService::extractImage` exactly as it
shipped, on Labeled Faces in the Wild (`lfw-funneled`; 300 identities with at
least four images, six images each, 1 560 faces; 3 391 genuine and 1.2 M
impostor pairs): genuine cosine mean 0.872, impostor mean 0.866, equal error
rate **48.8 %** — a coin flip. At the 0.80 threshold the search applied, 79 %
of impostor pairs matched, so a face login accepted almost any face as some
enrolled user, and the camera matcher's "known" verdicts meant nothing.

The cause was the alignment in `extractFace`: `ncnn::Mat::from_pixels_roi`
returns a planar *float* `Mat` of the face box, which was handed to
`warpaffine_bilinear_c3` as interleaved `uint8` pixels; the landmarks were in
full-image coordinates while the warp's source was the box; and the warped
interleaved buffer was read back as if it were planar. The recognizer
(MobileFaceNet, 128-d, normalisation inside the graph) received noise with a
little of the face's brightness in it.

`FaceService::alignFace` now warps the full interleaved RGB image straight
into the 112×112 ArcFace template (similarity transform from the five
landmarks, inverted for ncnn's warp) and the recognizer reads it with
`Mat::from_pixels`. `identity-face-model-test` pins it twice: a synthetic
image whose pixels encode their own coordinates must come out of the warp at
exactly the coordinates the landmark transform names (no model needed), and
the public-domain NASA crops in `tests/fixtures/face/` must separate (same
person 0.90 and 0.71, every cross pair ≤ 0.15).

Same LFW set after the fix:

| | Before | After |
|---|---|---|
| EER | 48.8 % | **3.2 %** (at 0.20) |
| TAR at FAR 1 % | 1.6 % | 96.6 % (threshold 0.25) |
| TAR at FAR 0.1 % | 0.2 % | 95.7 % (0.34) |
| TAR at FAR 0.01 % | 0.09 % | 93.2 % (0.42) |
| TAR at FAR 0.001 % | 0 % | 81.8 % (0.53) |
| impostor mean / p99.99 | 0.866 / 1.000 | 0.015 / 0.417 |

**The threshold.** `face.match_threshold` (default **0.50**, clamped
0.30–0.95) replaces the hard-coded 0.80, and enrollment's duplicate check
reads the same value, because "already registered" signs the person in. A
login is a 1:N search, so its false-accept rate is the per-pair rate times the
enrolled people: 0.50 is 2·10⁻⁵ per pair on LFW, about 2·10⁻⁴ per attempt in a
ten-person household, for 87 % TAR on LFW's unconstrained poses (a phone
selfie scores higher: the fixtures' pairs sit at 0.71–0.90). Security first:
a refused face falls back to the QR approval from a paired device, an
accepted stranger is a break-in. Live check on the sandbox: a throwaway
account enrolled from `barratt-a.jpg` signed in with `barratt-b.jpg`, and 0
of 40 LFW strangers plus the other fixtures did.

**Two embedding spaces are never compared.** `face_embedding.model` (added at
boot by `FaceEmbeddingRepository::ensureModelColumn`, `'legacy'` for every
row written before) names the space a vector lives in; new rows carry
`kFaceModelId` (`shared/vocabulary/face-model.hxx`). `FaceDB::init` drops
every `face_vec` row whose canonical row is gone *or* belongs to another
model, so a legacy vector can never be matched. The feature `face-upgrade`
then re-embeds, once per account, every user whose person has only legacy
rows, from the enrollment portrait already kept in private storage, and
indexes the new vector. Nothing is deleted: legacy rows stay in the table
(unindexed), portraits and users are untouched. An account without a usable
portrait is logged and signs in by QR until its face is registered again;
unpromoted camera candidates are not re-embedded (they have no portrait) and
expire through the candidate retention as before. Verified on a copy of the
sandbox database and then live (a legacy row re-embedded from its portrait in
S3, the account signing in afterwards).

`tools/face-calibration/` (`argus-face-calibration`, not built by default)
prints, per image, the detector score, face size, inter-ocular distance,
yaw, pitch, sharpness and the embedding, which is how the numbers above were
produced.

## Privacy choices and the household switches (2026-10-04)

David asked for consent before anything is processed: a notice at host
provisioning (the scripts) and, per person, in the app's first run, before any
face, voice or presence processing for that person. Identity owns people data,
so it owns the record. The feature is `src/feature/privacy/`; the table, the
gate and the vocabulary are in `src/shared/` because the RPC services, the
voiceprint feature and the privacy feature all read them (rule 23's 2+ rule).

**What is stored.** `user_privacy` holds one row per person: the notice
version they accepted (`kPrivacyNoticeVersion` in
`shared/vocabulary/privacy-choices.hxx`, which covers the privacy notice and
the pre-beta terms together) and four booleans: `presence`, `face_cameras`,
`voice_learning` and `camera_audio`, with `decided_at`/`updated_at`.
`household_privacy` holds a single row (id 1) with the Owner's household-wide
switches for the same four signals (default on) and `visitor_recognition`
(default off, read by the STRANGERS re-identification feature through
`PrivacyGate::household()`). Turning visitor recognition on requires
`acknowledge: true` and stamps `visitor_ack_at/by/version`; turning it off
keeps them. Both tables are `CREATE TABLE IF NOT EXISTS`, applied at boot like
every other table, so the change is additive.

**Effective value = the person's choice AND the household switch.** No row
means undecided, and undecided means every signal is off for that person.
`PrivacyGate` (`shared/services/privacy/`) is the one place that computes it.
A notice version older than the current one stays effective (the choices were
made), but the app asks again (`current: false`). The Owner can turn a signal
off for everyone, but cannot turn anything on for someone else: no route
writes another person's row.

**What enforces it.**
- Voice: `PassiveEnrollmentService::learnFromTurn` returns before analysing a
  turn when the speaker's account has no voice consent, so no call opens in
  the tracker; `recordCall` refuses with `NotConsented` before touching
  `voice_device`; `VoiceprintFeatureService::identify` answers no match for a
  person without it. Withdrawing (or the household switch going off) erases the
  profile, the samples and the device prior in the same transaction as the
  consent write (`eraseForConsent`, journaled as `voiceprint_forget` with
  `reason: consentWithdrawn`), and the `voice_vec` rows go after the commit.
  Existing voice profiles of undecided people are kept untouched but no longer
  match; they go the moment the person says no.
- Faces at cameras: `IdentifyPerson` with `purpose = IDENTIFY_PURPOSE_CAMERA`
  and `GetPerson` stop returning `user_id`, name, alias and observation for the
  person of a user without `face_cameras`, but keep `trusted` and the role. A
  household member is never reported as a stranger, and a sighting cannot
  name them or feed presence. Face login uses the default purpose
  (`IDENTIFY_PURPOSE_LOGIN`) and is never gated: the face is how a person
  signs in, which the first-run notice states.
- Presence (guard, PRESENCE) and camera audio (camera) read the effective
  values through `GetUser` (`UserIdentity.privacy`, field 7) and
  `ListPrivacy`, and react to the change feed: every consent decision, and
  every household flip (for every user), publishes the user's catalog row on
  the change subject with `row.privacy` = `{noticeVersion, decided, presence,
  faceCameras, voiceLearning, cameraAudio}`. argus-sync acks identity catalog
  events without forwarding them, so nothing reaches the apps; argus-auth only
  drops its cache entry. A user event without `row.privacy` is not a consent
  change.

**Audit.** Each decision journals one `UserAction` on `TableName::User`
(`event: privacy_consent`, the four booleans, the notice version, `changed`,
`first`, `byOwner: false`); each household flip journals `household_privacy`
with the switches and `changed`. Choices are audit-relevant facts, not
secrets; no image, vector or address is ever in them.

**Surfaces.** `GET /privacy/me` and `PUT /privacy/me` (every role, own row;
the body carries all four booleans and the `noticeVersion` the app showed, and
a different version answers 409 `PrivacyNoticeOutdated`), `GET /privacy/users`
and `PATCH /privacy/household` (Owner, `kPrivacyAccess` in role-access). gRPC
`ListPrivacy` (fleet-gated, active users plus the household row) and
`ListUsers` (every non-deleted user ordered by id, for guard's recipient
lists) were added beside `GetUser`.

`identity-privacy-test` pins the route access, the DTO refusals, the undecided
default, the outdated notice, the published and journaled decision, the
voice erase on withdrawal, the household switch with the unnamed `GetPerson`
answer, `GetUser`/`ListPrivacy`/`ListUsers`, the visitor acknowledgement and
`recordCall`'s refusal. `identity-voiceprint-test` seeds consent for its
people, since learning now requires it.

## Recurring visitors: faces that come back get a number, the Owner gives them a name (2026-10, STRANGERS)

David's words: "guardar los rostros desconocidos, como vecino: por algo existe el
person, que relaciona un usuario o una persona con varios rostros". A `person`
was already "one user or one stranger, many faces"; this feature makes the
stranger half real. The household is the persons with a `user_id`; a
**visitor** is a person without one. An unnamed visitor is "Persona #N"
(`person.visitor_number`, allocated once, never reused); the Owner can name it,
give it a type (`person.category`: `neighbor`, `delivery`, `service`, `family`,
`acquaintance`, `watchlist`, the `PersonCategory` wire enum in
`packages/contracts/identity`) and a note.

### Off by default, Owner's acknowledgement first

Nothing in this section runs until the Owner turns on
`household_privacy.visitor_recognition` with the acknowledgement
ONBOARD-CONSENT records (who, when, notice version; Ley 29733 and the
videovigilancia directive: notice sign, limited retention). Off means: no
visitor is created, matched or learned; the household is still recognised;
`EnrollPerson` answers `FAILED_PRECONDITION`; and the next retention sweep
(within 6 h) deletes every unnamed visitor. Named visitors are kept but are not
matched while it is off.

### One sighting, end to end

argus-camera's matcher sends the person crop with `IDENTIFY_PURPOSE_CAMERA`
and a `SightingContext {camera_id, observed_at}` (`identifyForCamera`).
`VisitorRecognitionService::observe` (`feature/visitor`):

1. `FaceService::analyzeImageAsync` on the **heavy lane**: detection, the
   aligned embedding, the quality geometry and, when recognition is on, a
   ≤192 px JPEG of the face itself (never the body crop).
2. On a `BlockingStrand` (one sighting at a time, so two crops of a new face
   cannot create two people): the 24 nearest `face_vec` rows, grouped by
   person and pooled as household / named / unnamed, go through the pure
   `visitor_policy::decide`:
   - **match gate** (detector ≥ 0.80, inter-ocular ≥ 12 px, yaw ≤ 0.35,
     pitch 0.25–0.85) or nothing happens;
   - household ≥ 0.50 and at least as close as any visitor → household (the
     user is reported as before, gated by their own `face_cameras` consent);
   - recognition off → stop;
   - household ≥ 0.30 (the **guard band**) → nothing is stored: a household
     member who matched badly is never turned into a visitor;
   - a visitor ≥ 0.55 with a margin of 0.08 over the runner-up → that visitor;
     inside the margin → ambiguous, nothing stored;
   - best visitor 0.35–0.55 → uncertain, nothing stored (no duplicate);
   - otherwise, and only past the **learn gate** (detector ≥ 0.90,
     inter-ocular ≥ 16 px, yaw ≤ 0.25, pitch 0.30–0.80) → a new visitor.
3. The sample is added (learn gate only) when its median similarity to the
   person's samples is ≥ 0.35 (outlier rejection) and either fewer than 8
   samples exist or it is better (`face_quality::score`: detector × size,
   sharpness, frontality) than the worst, which it replaces (best K = 8).
   A near duplicate (≥ 0.92 to an existing sample: the same frame seen
   again) never takes a second slot; it replaces that sample only when it is
   sharper.
4. The visit: sightings of one person on one camera less than 10 min apart are
   one `person_visit` row (`sightings` counts them); a new row raises
   `person.visit_count`.
5. The face crop goes to private object storage (`faces/<person>/<sample>.jpg`,
   `face_embedding.crop_key`), and an evicted sample's crop is removed.
6. **Household echo**: when a household sighting also lands ≥ 0.55 on an
   unnamed visitor, that visitor was the household member leaking through the
   guard band; it is deleted with its samples, crops and visits.

Camera samples are **never** added to a household person: a household person's
vectors are what face login matches, and a wrong camera sample there would let
someone else sign in.

### The thresholds, measured

Same LFW subset as the fix above, run through the full pipeline after the
detector letterboxes instead of stretching to 640×640 (a tall person crop was
squashed 3× sideways: at 14/18 px between the eyes the stretched detector found
73 %/80 % of the faces, the letterboxed one 100 %, and the EER fell from
18.0 %/15.8 % to 13.2 %/10.3 %). "CCTV" probes are the LFW faces scaled to an
inter-ocular distance of 14, 18, 24 or 32 px inside a person-shaped crop,
Gaussian blur σ 0.6, JPEG 55 (stream) then 85 (the matcher's encode) —
`tools/face-calibration` plus scratch scripts. Household = one clean
enrollment per identity, probes = CCTV images of the same and other people:

| probe iod | household TAR at 0.50 | FAR/pair at 0.50 | leak below 0.30 (learnable) |
|---|---|---|---|
| 18 px | 50.6 % | 3·10⁻⁶ | 8.3 % |
| 24 px | 71.5 % | 3·10⁻⁶ | 6.6 % |
| 32 px | 79.2 % | 5·10⁻⁶ | 7.8 % |

Visitor against visitor (both CCTV):

| probe iod | TAR at 0.55 | FAR/pair at 0.55 | genuine below 0.35 (would split) |
|---|---|---|---|
| 18 px | 33.3 % | 2·10⁻⁶ | 20.8 % |
| 24 px | 51.7 % | 2·10⁻⁶ | 14.7 % |
| 32 px | 63.3 % | 8·10⁻⁶ | 13.3 % |

Reading them: the visitor pool is searched 1:N, so 0.55 keeps a false join
around 10⁻³ per sighting with a few hundred visitors, while a missed join
only costs a duplicate the Owner can merge — precision first. The leak
column is LFW's hard pairs (years apart, other poses); same-day camera
faces of a household member score higher, and the echo rule removes the
residue. Below 12 px the model is near chance (EER 25 % at 10 px), hence the
match gate; 16 px is where it gets usable (EER 4.5 % on the CCTV set), hence
the learn gate. Sharpness is computed and logged but not gated: at these sizes
it only restates resolution.

### Owner surfaces (HTTP 7044)

| Route | Who | |
|---|---|---|
| `GET /visitor[?scope=named&filter=&q=&limit=&beforeSeen=&beforeId=]` | Owner all; Guard named only | one keyset page of the gallery (see "Paging the gallery") with visit count, first/last seen, cameras, best sample, and `nextCursor` |
| `GET /visitor/{id}` | Owner; Guard named only (no visit history) | samples, last 200 visits, visit pattern |
| `PATCH /visitor/{id}` `{name?, category?, note?}` | Owner | naming makes it `known` (trusted unless `watchlist`); an empty name returns it to the unnamed pool and its retention |
| `POST /visitor/{id}/merge` `{sourceIds}` | Owner | samples and visits move, sources are deleted, the result is trimmed to the best 8 |
| `POST /visitor/{id}/split` `{sampleIds}` | Owner | the samples become a new "Persona #N" with one visit per sample |
| `DELETE /visitor/{id}`, `DELETE /visitor/{id}/samples/{sampleId}` | Owner | embeddings, crops, visits and capabilities go (hard delete: visitors are not synced) |
| `GET /visitor/{id}/crop-preview[?sampleId=]` → `GET /visitor-crop/{token}/content` | Owner; Guard for named | the portrait pattern: a 60 s, requester-bound, one-use capability, consumed before the object is read, journaled as `UserAction::Read` |
| `GET/PATCH /visitor-settings` `{unnamedRetentionDays}` | Owner | 1–60 days, default 30 (`visitor_setting`) |

Household persons never appear here, and merges or splits with one are refused
(they are not visitors). Every name/type change, merge, split, delete and
crop view is a journal event with safe metadata only (`named`, `category`,
counts — never a vector, a key or a picture).

### Paging the gallery (2026-10, INFINITE)

The gallery used to answer up to 500 visitors at once and filter and search
them on the phone, so a busy place (a shop, a restaurant with a 30-day
window) could have visitors the app never showed. `GET /visitor` now pages:
`limit` (1-500, default 500 so an old client keeps its answer), `filter`
(`all`, `named`, `unnamed`, `watchlist`), `q` (up to 64 characters, matched
in the name, the note and the visitor number) and the keyset cursor
`beforeSeen` + `beforeId`. Rows come in the order they always had,
`last_seen_at DESC, id DESC`, and the next page starts strictly after the
`(last_seen_at, id)` pair of the last row. That pair is a total order, so two
visitors seen in the same second are never skipped or repeated across a page
boundary. `nextCursor` is `{lastSeenAt, id}` while more rows exist and `null`
on the last page. The service asks the repository for `limit + 1` rows to
know which, so the client never has to guess from a short page.

The cursor is a row value, `(p.last_seen_at, p.id) < (?, ?)`, and the first
page passes the largest integer for both. With the partial index
`idx_person_visitor_seen (last_seen_at DESC, id DESC) WHERE deleted_at IS NULL
AND user_id IS NULL` every page is a range seek that stops at the limit
(`SEARCH p USING INDEX idx_person_visitor_seen (last_seen_at<?)`). A
`? = 0 OR ...` cursor would have scanned the skipped rows on every deep page.
The filter is the typed `VisitorListFilter` (rule 1's shape: its own
`ToString`/`FromString`, used only at the SQL boundary). The search is lowered
in ASCII on both sides (`lower()` in SQLite, the same in C++), so an accented
capital (Á, É) matches only itself. The app sends its query already in
lowercase, so this covers what people type.

### Retention and sync

`CandidateRetentionService` now retires **unnamed visitors**
(`user_id IS NULL AND name = ''`) unseen for the Owner's
`unnamed_retention_days` (30 by default, never more than 60 — the directive's
ceiling), or all of them while recognition is off, together with their
samples, crops, visits and capabilities. `[retention] candidate_days` is gone.
Named visitors stay until the Owner deletes them. The `person` sync pull and
its live adds now carry household persons only: visitors are Owner data behind
HTTP, never on Resident or Guard phones.

`GetPerson` adds, for a visitor, `category`, `visits`, first/last seen, the
number and `VisitPattern` (weekdays reached by ≥ 30 % of the last 60 visits,
and the usual hour when the middle half of the visits lies within 3 h), which
guard turns into "es el repartidor que suele venir los martes".

### Tests

`identity-visitor-policy-test` (pure: every branch of `decide`, best-K and
outlier admission, the weekday/hour pattern) and `identity-visitor-test` (live
loop, real model, the NASA fixtures as camera crops: off → nothing; a first
sighting creates Persona #1; another photo two minutes later joins it without
a new visit; the next day is a new visit; the household member is recognised
and gets no camera sample; Owner names, merges, refuses a household merge,
splits, deletes; the index stays in step with the canonical rows; turning
recognition off purges the unnamed and stops matching; the gallery pages two
at a time with a cursor that ends on the last page, and the filters and the
search narrow it on the server).
