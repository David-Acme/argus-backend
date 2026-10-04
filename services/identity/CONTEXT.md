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

## Voiceprints (speaker verification)

A person's voice is linked to them **once**, and only under a confirmed
enrollment; afterwards the service verifies (1:1) and identifies (1:N) them by
voice. The feature is `src/feature/voiceprint/`, and it mirrors the face
design: the engine runs in this process, the canonical row stores only an
embedding, and a vec0 table (`voice_vec`) is the search index.

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

The operating points come from the same runs. 1:1 (`verify_threshold` 0.50):
3-second clips give a false-accept rate of 0.36 % (EN) / 0.50 % (ES) at a
false-reject rate of 2.3 % / 0 %; 2-second clips 0.28 % / 0.31 % at 5.7 % /
3.3 %. 1:N (`identify_threshold` 0.55, `identify_margin` 0.05 over the
runner-up) is stricter because a search over N people multiplies the
false-accept rate by N. Enrollment consistency (`consistency_threshold` 0.50,
each sample against the centroid of the others) rejected no genuine set and
accepted at most 1.5 % of sets with one sample from another speaker.

### What is stored

`voiceprint` (one row per user, `UNIQUE user_id`): the model id (the model
file's stem), the L2-normalised centroid as a float32 BLOB, the sample count,
the voiced seconds, the method (`self` or `owner_face`), the consent version
the person accepted and who enrolled it. `voiceprint_challenge` holds the
enrollment challenges, hash-only like the invitation and portrait tokens (the
256-bit token lives in `shared/services/token/opaque-token`, which the
invitation and portrait-preview features now share), and
`voiceprint_challenge_sample` the per-phrase embeddings an enrollment in
progress has staged, which die with their challenge. Raw audio never reaches a
table, a file, a log or the change feed: it is decoded in memory, measured,
embedded and dropped. The `voice_vec` index is rebuilt from the rows of the
active model at every boot (it holds a household's handful of rows), so a stop
between a commit and its index write repairs itself.

**Versioning.** A row whose model id differs from the configured model is
*stale*: excluded from the index, reported as `stale` by the status, and the
only voiceprint that may be enrolled over without deleting it first. Swapping
the model therefore never compares embeddings from two different spaces.

### Confirmed, once

An enrollment is accepted only when all of these hold, checked in this order:

1. **Who.** The subject is the authenticated caller (`JwtContext.sub`), or
   the caller is an Owner. Any other combination is `Forbidden`; an inactive
   or deleted subject is `UserNotFound`.
2. **Consent.** `consent=true` together with the consent version the server
   currently presents (`voiceprint-consent-v1`); the version is stored.
3. **Not already linked.** A current-model voiceprint answers
   `AlreadyEnrolled` (409). Re-enrolling is an explicit delete followed by a
   new enrollment, both audited, so a hijacked session cannot silently
   replace somebody's voice.
4. **A live challenge.** `POST /voiceprint/me/challenge` mints a one-use,
   five-minute challenge bound to the subject, the requester and the device
   hash, with three random phrases from a static per-language bank. The
   enrollment must present it from the same device; it is consumed in the
   same transaction that writes the voiceprint.
5. **Owner-assisted only with the face.** When the Owner enrolls someone
   else, the request must carry a face image taken in the same request, and
   `FaceService` must identify it as the person linked to that user
   (`FaceNotVerified` otherwise).
6. **Quality.** Each sample is decoded (WAV PCM 8/16/24/32-bit or float, any
   rate 8-48 kHz, any channel count, at most 30 s) and resampled to 16 kHz
   with `lib/audio`'s `AudioResampler`. An energy analysis over 20 ms frames
   measures voiced seconds, an SNR estimate (loudest half against the
   quietest tenth) and clipping; a sample under 1.2 s of speech, under 12 dB
   or with more than 1 % clipped samples is refused with its index. The
   embedding is taken over the voiced span only.
7. **One speaker.** The consistency check above.
8. **Not somebody else's voice.** If the centroid matches another user's
   voiceprint above the identification threshold the enrollment is refused
   (`VoiceAlreadyLinked`): nobody can link a recording of a housemate to their
   own account.

Deletion is the subject's or an Owner's (`DELETE /voiceprint/me`,
`DELETE /voiceprint/user/{id}`), hard (a biometric must be erasable), and works
for an inactive user. Enrollment and deletion each publish one
`UserAction::Create` / `UserAction::Delete` journal event on `TableName::User`
through the identity change sink, inside the same transaction, with safe
metadata only (`event`, `method`, sample count, model, consent version,
`replaced` / `byOwner`). Nothing about voiceprints enters the sync stream: the
app reads its own status over HTTP.

### Threat model

- **Replay of an old enrollment upload** fails: the challenge is one-use,
  short-lived and device-bound.
- **A recording of the victim enrolled into the attacker's account** needs
  the attacker's own session, is refused when the victim is already enrolled
  (rule 8), and leaves an audit trail. The challenge phrases are the hook for
  the remaining gap: once the spoken content is checked against them, a
  recording of anything else fails. That check belongs to speech recognition
  and is not done yet — see below.
- **Replay or synthetic speech at verification time** is not detected; there
  is no anti-spoofing model (the open ones, e.g. AASIST, are trained on
  ASVspoof logical-access attacks and are unreliable on phone-microphone
  replay). Therefore a voice match is a **soft identification signal**: who is
  speaking in a call, a greeting, attributing a turn. It is never an
  authentication factor — there is no voice login, and no consumer may unlock,
  arm, disarm or authorize anything on a voice match alone.
- **The biometric itself** stays on the host: only the centroid is stored, it
  never leaves this process (the RPCs answer scores and ids, never vectors),
  and the HTTP and gRPC bodies are not logged.

### Liveness: the part left to speech recognition

The challenge already carries what a spoken-content check needs: the phrases
are stored with the challenge (JSON, per language). Completing it is an STT
call per sample — transcribe, normalise, require a fuzzy match (for example a
word error rate under 0.4) against the phrase shown — before the embedding is
accepted. identity can do that through `argus::clients::stt` without any change
to argus-stt; it is not wired today because argus-stt is not part of the
default native stack and an unreachable STT must not block enrollment. The
voice service, which already transcribes every turn, can apply the same check
when it drives an enrollment by voice.

### Surfaces

HTTP (TLS 7044, the app) is JSON end to end — audio travels as a
base64-encoded WAV, because the desktop's request path reads multipart files
from disk and a recording made in the WebView never is one:

- `GET /voiceprint/me` — status (`available`, `enrolled`, `stale`, model,
  sample count, method, consent version, `samplesRequired`,
  `minSpeechSeconds`).
- `POST /voiceprint/me/challenge` `{lang?}` — the challenge: id, phrases,
  expiry, consent version.
- `POST /voiceprint/me/sample` `{audio, challengeId?, phrase?}` — without a
  challenge, quality feedback only; with one, the phrase is analysed and its
  **embedding** (never the audio) staged in `voiceprint_challenge_sample` at
  that position, replacing an earlier take. The answer says `accepted`,
  `problem` (`too_short`, `too_noisy`, `clipped`, `invalid`), the measured
  speech seconds and SNR, and `collected`/`required`, so the app gives
  feedback phrase by phrase.
- `POST /voiceprint/me` `{challengeId, consent: true, consentVersion}` —
  runs the gates over the staged embeddings and links the voiceprint; the
  challenge and its staged rows are consumed in the same transaction, and
  expired challenges take theirs with them at the next challenge.
- `POST /voiceprint/me/verify` `{audio}` — "try my voice".
- `DELETE /voiceprint/me`.
- For the Owner, behind `RoleFilter`: `GET|DELETE /voiceprint/user/{id}`,
  `POST /voiceprint/user/{id}/challenge`, `POST /voiceprint/user/{id}/sample`
  and `POST /voiceprint/user/{id}` (the last also takes `face`, a base64
  JPEG of the person taken during the enrollment).

The `/me` routes need no role entry: every authenticated role manages its own
voice, the same shape as `/auth/me`. `/voiceprint/user/...` maps to no table in
`role-access`, so only the Owner passes `RoleFilter`.

gRPC (`argus.identity.v1.VoiceprintService`, fleet-secret gated, client
`VoiceprintClient` in `argus::clients::identity`; `Enroll` takes every clip
in one call, since the voice relay already holds them): `Verify`, `Identify` and
`GetStatus` for the voice relay and guard; `CreateChallenge`, `Enroll` and
`Delete` additionally require the person's bearer token and device hash,
validated through argus-auth exactly like `PromotePerson`. Every answer carries
a `VoiceprintOutcome`.

Measured on this machine (Debug build): one 3-second clip is decoded,
analysed and embedded in about 100 ms on two threads; extraction is bounded
by `ThreadBudget::inferenceSlots()` like the face engine.

### Tests

`identity-voiceprint-audio-test` (no model): WAV decoding (mono, stereo,
float, 24-bit, extensible, truncated, refused shapes), raw PCM, resampling,
the speech-quality analysis on synthetic signals, the vector maths and the
phrase bank. `identity-voiceprint-test` (live Drogon loop and database, the
real model and the LibriSpeech fixtures in `tests/fixtures/voiceprint/`):
same-speaker against different-speaker scores, the whole gating order
(forbidden, unknown and inactive subjects, consent and its version, a
challenge from another device, sample count, mixed speakers, a silent sample,
already enrolled, a voice already linked, owner-assisted without a verified
face), the app's staged flow (a phrase per call, a silent take refused, a
position out of range, another device, too few phrases, a mixed set named by
position, the re-take, the staged rows gone after the commit), verify,
identify, delete by the Owner, the audit events, and the gRPC
surface through `VoiceprintClient` with a scripted auth verdict. Without the
model on disk the model-backed half reports itself skipped.
