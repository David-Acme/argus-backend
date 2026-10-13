# The Argus-side recorder: design note (F1)

Status: **ON HOLD by the owner (2026-10-13)**: he wants to clarify some points before anything is
built, because the feature is new. The design was approved by the reviewer the same day, with the
rulings recorded in place (marked RULED). Nothing here is implemented; nothing starts until the owner
has given his clarifications and the note is updated with them. The requirements
are the owner-approved spec in `realtime-and-backlog-plan.md`, F1, "Argus-side recording,
owner-approved spec and rulings (2026-10-13)" (master `30e8e131`); this note answers how, not what.
Every item gives the options, a recommendation and its reasons. **DESIGN FORK** marks a decision that
needs the reviewer.

**Rulings (reviewer, 2026-10-13).** The shape is approved: a separate `argus-recorder` with no public
listener, ffmpeg stream copy over go2rtc's RTSP restream, guard's episode and safety_alert subjects to
the durable consumer `argus-recorder-guard`, and HLS through argus-camera with 10-minute view tokens.
The six forks are ruled in place: 1 (b), 2 (a), 3 AAC-LC 64 kb/s mono by default (revised 2026-10-13;
FLAC is an optional later upgrade behind a four-player gate), 4 (a) with a 7-day deletion seal, 5 (a)
with the viewed range written at session end, 6 (a).

## 0. Measurement: stream1 per camera

The measurement was not taken. Its preconditions failed: go2rtc was not running on the host (no
`go2rtc` process, nothing listening on 1984 or 8554), argus-camera was not running either, and the
note's rules forbid starting go2rtc, any Argus service or a container to measure. No camera was
contacted. The known facts come from the read-only device probe already recorded in
`services/camera/CONTEXT.md` ("What the Tapo encoder offers"); the camera set itself is to be read
from camera.db when the measurement runs.

| Camera | Codec | Resolution | GOP (s) | Bitrate (Mbps) |
|---|---|---|---|---|
| Patio (Tapo C225, stream1) | H.264, 15 fps, PCMA 8 kHz audio | 2688×1520 | PENDING | PENDING |
| Every other configured camera | PENDING | PENDING | PENDING | PENDING |

**Working assumption until measured: a 2 s GOP and 2 Mbps on stream1.** Every number below is
derived from it and is recomputed from the measurement.

How to take it, once go2rtc runs:
- A read-only `ffprobe -rtsp_transport tcp -loglevel error -timeout 5000000 -read_intervals %+20
  -select_streams v:0 -show_entries packet=pts_time,flags,size` through go2rtc's RTSP restream of
  `cam<id>` (main), with the URL kept out of every output.
- The keyframe spacing comes from the `K` flags, and the bitrate from the summed packet sizes over
  the window.
- The same run over ten minutes, and over a night with IR on, gives the RTSP stability and the VBR
  peak the ring budget needs (section 3).

**Ruling (2026-10-13):** PENDING is accepted, with the 2 s GOP and 2 Mbps working assumption. Starting
go2rtc is the owner's step; the reviewer raises it with the owner.

## 1. The recorder's home

**Options.**

- **A. A new service, `services/recorder` (`argus-recorder`).** It owns `recorder.db`, the ring
  directory, promotion to RustFS, retention and quotas, and the access log.
  - `recorder.db` is the index: ring segments, clips, manifests, gaps, backfills, locks, the access
    log, the guard-event inbox and its change outbox.
  - It reads video from argus-camera's go2rtc and asks argus-camera for SD-card backfills over gRPC.
  - It has no public listener. The app's playback goes through argus-camera (section 6).
- **B. A second executable of the camera project (`argus-camera-recorder`), its own process.** It
  reuses camera code directly (the Tapo client for the SD backfill, the go2rtc helpers,
  `Fmp4Reader`) and either shares `camera.db` with argus-camera or opens a `recorder.db` of its own.

**What each costs.**

| | A. `argus-recorder` | B. second camera process |
|---|---|---|
| Containers | +1, its own image (Debian trixie-slim + ffmpeg) | +1 from the camera image; one container with two processes would need a supervisor and dies with argus-camera |
| Mounts | `${ARGUS_DATA_DIR}/recorder` (db + SSD ring), a sized `tmpfs` for the ring, `config.recorder.toml` (0600), the signing key (ro), its `schema.sql` (ro) | the same, plus everything the camera image already mounts |
| NATS | a new `recorder` user in track B's allow-list table (section 5) | the camera user widened, or a second user for the same project |
| Databases | +1, `recorder.db` (rules 26 and 27 hold as they are) | `camera.db` written by two processes (lock contention, one schema with two writers), or a `recorder.db` that makes B a separate service in all but name |
| Ports | 7048, internal gRPC only (playback reads, settings wire, module data and purge) | the same, on the camera project |
| go2rtc | argus-camera exposes go2rtc's RTSP listener on the internal network with an RTSP username and password; no host port | loopback inside one container, or the same exposure across two |
| Object storage | its own S3 credential scoped to `recordings/` (S4) | the camera credential, widened to recordings |

**Recommendation: A, a separate service.** The reasons:
- Custody is its own domain. The access log, the evidence lock, signed manifests and the retention
  of footage are evidence custody, not device control. A service that owns them alone is what rule
  27 already describes: one database, one schema, one NATS user, one storage prefix.
- It needs none of argus-camera's secrets. The recorder never holds a camera password or the Tapo
  cloud account: it reads a restream and asks for SD backfills by RPC.
- No HTTP surface runs in the recording process (section 6). Restarts and deploys are independent:
  an HTTP fix in argus-camera never stops a segmenter.
- B's one real saving, calling the Tapo client in process for the SD backfill, is better spent as an
  RPC. argus-camera owns the camera's connection budget (below), so the arbitration belongs there.

**What a separate process does not buy, stated honestly.** go2rtc lives inside argus-camera
(`Go2rtcManager` spawns and supervises it), so an argus-camera restart restarts go2rtc and cuts the
recorder's source for its duration, whichever option is chosen. The spec's "a backend restart does
not stop recording" then holds for every service but argus-camera; for argus-camera it becomes an
explicit gap in the manifest, filled from the SD card (section 4).

**DESIGN FORK 1: go2rtc's lifecycle. RULED: (b).**
- (a) Keep go2rtc in argus-camera for v1, accept explicit gaps on camera restarts and measure how
  often they happen (recommended: no re-plumbing, and the gap is visible and backfilled).
- (b) Move go2rtc into a container of its own that argus-camera drives over its API, so a camera
  restart no longer touches the stream. This means re-plumbing the source registrar, WebRTC
  admission and the 8555 port, and putting go2rtc's API, which serves the camera URLs with their
  credentials, on the internal network.

Recommendation, superseded by the ruling: (a) now, (b) only if the restart count says so.

**Ruling: (b).** go2rtc runs in its own container or process, driven over its API by argus-camera.
It keeps the same image and config, and argus-camera keeps ownership of the config. Recording
continuity is the promise of this feature: a routine argus-camera restart must not cut every camera's
recording, and (a) would turn every deploy into a gap. go2rtc's API stays on the internal network with
no host port.

**The camera's connection budget changes.** Today stream1 is pulled only while someone watches it,
which keeps a Tapo camera inside its three connections (sub preload, main viewer, talk line). An
always-armed ring keeps main open all the time. It is still one connection, because go2rtc shares
its main upstream between the recorder and every viewer. An SD backfill (the 8800 `sdvod` stream)
needs one more, so it can only run while the talk line is idle.

**DESIGN FORK 2: the third slot.** argus-camera arbitrates it. RULED: (a).
- (a) The talk line always wins, and a backfill waits and retries (recommended: a live conversation
  is time-critical, and a backfill is not).
- (b) A backfill pre-empts talk.

**Ruling: (a).** The talk line wins, and an SD backfill waits and retries with a backoff. A live
interaction outranks a backfill that can happen later.

## 2. The segmenter

**Options.**

- **ffmpeg stream copy from go2rtc's RTSP restream**, one ffmpeg per camera, supervised by the
  recorder.
  - The flags: `-rtsp_transport tcp` and `-c:v copy`, with the HLS muxer writing fMP4
    (`-hls_segment_type fmp4 -hls_time 4 -hls_flags independent_segments+temp_file -hls_list_size
    0`) and segments named by sequence.
  - The recorder reads each completed segment after ffmpeg's rename, hashes it, indexes it and owns
    deletion; ffmpeg never deletes.
- **An in-process fMP4 writer** that reads go2rtc's `/api/stream.mp4?src=cam<id>&mp4=flac`, cuts
  segments at video keyframes and writes go2rtc's `moof`/`mdat` boxes verbatim.

**Reuse, evaluated honestly.** argus-camera's stream code holds the right knowledge, but none of it
is a recorder.
- `Fmp4Reader` is the valuable piece. It finds the video track in `moov` and classifies each
  fragment by the track its `traf` carries. That matters for go2rtc's fMP4, where every audio sample
  is flagged as a sync sample, so a naive cutter would split on audio. It lives in
  `services/camera/src/shared/services/stream/`, so a separate service could use it only after a
  move into a package (`packages/lib/fmp4`, tier 1) and a re-link of camera.
- `GopCache` is a live-view cache: one GOP for a fast start, capped at 2 MiB and dropped past the
  cap. A ring of minutes on disk is a different structure.
- `StreamHub` is a viewer fan-out with credit windows and a stall timeout, inside argus-camera's
  process: the restart domain the spec wants recording out of.
- The in-process path needs go2rtc's HTTP API on the internal network, and that API also answers
  `/api/streams` with every camera URL and its credentials. The RTSP listener exposes only the
  restreams, behind its own username and password.

**Recommendation: ffmpeg stream copy over RTSP.** The reasons:
- The keyframe-aligned cut, timestamp continuity, the `temp_file` write-then-rename and the RTSP
  reconnect behaviour are ffmpeg's tested code, not new parsers of ours.
- The exposure is narrower: RTSP restreams only, not go2rtc's API.
- ffmpeg is already in the camera image (go2rtc uses it for `frame.jpeg`, and `audio-capture.cc`
  spawns it), so the recorder image takes the same Debian package.
- One process per camera isolates failures. A crashed ffmpeg is one camera's gap, restarted with a
  backoff, and its exit and restart times are exactly the gap the manifest records.
- The costs: about 15 MB RSS and 1-3 % of a core per camera for a copy, and segments of "about
  4 s" in the strict sense. ffmpeg cuts at the first keyframe at or after 4 s, so a 2 s GOP gives
  4 s segments and a 5 s GOP gives 5 s segments.

**Audio.** The C225 sends PCMA 8 kHz, which ISO BMFF (and so fMP4 and HLS) cannot carry. The video
stays a byte-exact stream copy; the audio becomes AAC-LC at 64 kb/s, mono, in the same ffmpeg pass,
transcoded from the 8 kHz A-law (ruling 3, revised 2026-10-13). A "no audio"
camera gets `-an`.

**DESIGN FORK 3: the audio codec. RULED: AAC-LC by default; FLAC an optional later upgrade.** The first
ruling (FLAC behind a playback gate) is revised below; the options stay as they were written.
- (a) FLAC (recommended): lossless over the decoded A-law, so every sample is the camera's, and the
  codec the live view already plays on web and Android. iOS playback of FLAC in fMP4 HLS stays
  unverified until the app ships there.
- (b) AAC: plays everywhere, but lossy.

**Revised ruling (2026-10-13): AAC-LC is the default.** AAC-LC at 64 kb/s, mono, plays everywhere without
a device check, and the audio is transcoded in both cases from 8 kHz A-law, so FLAC's losslessness buys
little evidence value. The video is never transcoded. FLAC becomes an optional later upgrade, behind the
same four-player gate below, run in the device-validation pass listed in the frontend CONTEXT.md device
checklist (item 8). Chromium and WebKitGTK are smoke-checked in the e2e harness for whichever codec ships.

| Target | Player | Result |
|---|---|---|
| Android | ExoPlayer / Media3 | PENDING |
| iOS | AVPlayer | PENDING |
| Desktop webview | WebKitGTK | PENDING |
| Chromium | Chromium | PENDING |

The gate has not run, and it gates only the optional FLAC upgrade. Each row is filled in when the device
pass runs, with the player version and the clip used. FLAC stays off until every row passes.

## 3. Where the ring lives: tmpfs or SSD

**The budget per camera**, at the working assumption (2 Mbps, 2 s GOP, 4 s segments, 5 min
pre-roll):
- A 4 s segment carries 1.00 MB of video, about 32 kB of AAC-LC audio (mono, 64 kb/s) and about 2 kB
  of fMP4 overhead: **1.03 MB**.
- The pre-roll is 75 segments, 78 MB. The ring also holds the segment being written, one completed
  and not yet hashed, and one of slack for a late keyframe: **81 MB**.
- Until the bitrate is measured, a VBR peak factor of 1.5 covers motion and night IR noise, which
  raise H.264's bitrate: **122 MB per camera** at 2 Mbps, about 240 MB at 4 Mbps. Once the
  recorder runs, the measured p95 of its segment sizes replaces the factor.

**The rule.**
- The ring's size: `ring = Σ cameras ⌈bitrate_p95 × (preRoll + 3 × segment) / 8⌉`.
- The ring goes to tmpfs when `ring ≤ min(10 % of MemTotal, MemAvailable − max(4 GiB, 25 % of
  MemTotal))`.
- Provisioning evaluates it (and sizes the compose `tmpfs:` mount), and so does the recorder, at
  boot and on every camera add, camera remove or change in the measured bitrate.
- Examples at 2 Mbps:
  - A 32 GB host (this one) takes 26 cameras in its 3.2 GB.
  - A 16 GB host takes 13 in 1.6 GB.
  - An 8 GB host takes 6 in 0.8 GB, provided 4 GiB plus the ring stays available beside the AI
    stack.
- tmpfs pages count against the container's cgroup, so the recorder's `mem_limit` is the tmpfs size,
  plus about 64 MB per camera (ffmpeg and buffers), plus 128 MB.

**The fallback is per camera.** A camera that does not fit, or whose write fails with `ENOSPC`,
moves its ring to `${ARGUS_DATA_DIR}/recorder/ring/cam<id>` on the SSD, and the storage dashboard
says so. The SSD is a sound fallback, not a degraded one:
- The ring writes sequentially, 0.25 MB/s per camera at 2 Mbps (21.6 GB a day).
- Four cameras write about 31.5 TB a year. A 600 TBW drive lasts about 19 years at that rate, and a
  150 TBW budget drive about 5.

That is why the spec puts SMART wear on the dashboard, and why tmpfs is preferred whenever it fits.

## 4. The manifest

**One clip is a chain of manifest versions.**
- A clip's manifest is rewritten as segments are promoted during the event (the spec: promoted
  during, not at the end), and each version is a new immutable object.
- Every manifest of a camera carries the SHA-256 of that camera's previous manifest, across clips,
  so removing or editing any of them breaks the chain.
- An expired clip leaves a signed tombstone in the chain (the deleted manifest's hash, the expiry
  date and the reason), so retention does not break verification.

**Fields** (JSON, UTF-8, stored and hashed as the exact bytes written):
- `format` (`argus.recording.manifest/1`), `instance` (the installation id), `camera {id, name,
  model, stream: "main"}`.
- `clip {id, version, openedAt, closedAt?, linkedFrom?, linkedTo?}`: a clip capped at 30 min rolls
  into a linked one, and the links are fields, not inference.
- `previous`: the SHA-256 of the camera's previous manifest bytes, or `null` for the first.
- `triggers[]` `{source: guard.episode|guard.safety_alert, id, eventId, danger, at}`: every event
  that opened or extended the clip.
- `clock {source: "server-ntp", syncedAt, cameraOffsetMs}`: server time is authoritative, and the
  camera's offset is recorded, never applied.
- `inits[]` `{id, sha256, bytes}`: a new ffmpeg run, after a camera restart or a resolution change,
  starts a new init.
- `segments[]` `{seq, init, start, duration, bytes, sha256, source: "live"}`.
- `gaps[]` `{start, end, reason, backfill}`:
  - `reason` is one of `upstream_stalled`, `camera_offline`, `go2rtc_restart`, `recorder_restart`
    or `trigger_late`;
  - `backfill` is one of `pending`, `done`, `unavailable` or `refused`.
  - A camera in privacy mode is a `privacy` interval, never a gap, and it is never backfilled.
- `backfills[]` `{gap, source: "sd", sdRange {start, end}, objects[] {key, bytes, sha256}}`.

**The signature.**
- A detached Ed25519 signature covers the exact manifest bytes, with `keyId` (the SHA-256 of the
  public key) beside it, so the verifier never canonicalises JSON.
- The instance key is new and separate from the tunnel link identity. Provisioning generates it
  0600 under `${ARGUS_DATA_DIR}/certs/recorder/`, and the recorder alone mounts it.
- The public key and its fingerprint ride in every export and on the Owner's storage screen.
- A rotated key keeps its predecessors' public keys in `recorder.db` and in every export bundle.

**The SD backfill is a second source, never merged.**
- A gap triggers a backfill RPC to argus-camera for that window (section 1, the third slot).
- The SD bytes are stored as received (decrypted MPEG-TS from `sdvod`), under their own keys and
  hashes.
- The manifest lists them beside the live segments instead of splicing them in. Playback may show
  them in the gap, but the two sources stay distinguishable in every export.

**Export** is the init plus the segments concatenated byte for byte (a valid fragmented MP4), the SD
objects as they are, every manifest version, the signatures, the public keys and a verify tool that
re-hashes every byte range.

## 5. How guard's events drive a clip

**What guard publishes today is not enough.** `argus.guard.v1.change` carries `{domain, scope,
ids}` notices that ask a screen to re-read: no camera, no danger, no phase.
`argus.guard.v1.encounter_closed` covers closes only.

**Recommendation: two new subjects on the stream guard already owns** (`ARGUS_GUARD`, filter
`argus.guard.v1.>`, so no broker or stream change). Guard publishes them through its change outbox,
in the transaction of the change they describe, each with a minted event id as the JetStream msg
id.
- `argus.guard.v1.episode`: `{eventId, episodeId, environmentId, cameraIds[], phase:
  opened|escalated|touched|closed, danger, subject, record, postRollS, at}`.
  - Guard decides `record` and `postRollS`: which events are important is danger policy, guard's
    domain (its AGENTS.md rule 1). The recorder only executes.
  - `touched` rides the existing `EpisodeChangeGate` (leading plus trailing, 15 s by default), so an
    open episode keeps its clip alive with at most one event per window.
  - A camera that joins an open episode (cross-camera continuity) starts its own clip with its own
    pre-roll.
- `argus.guard.v1.safety_alert`: `{eventId, alertId, kind: panic, environmentId, cameraIds[], at,
  postRollS: 600}`. A panic names no camera, so guard lists the environment's cameras.

**DESIGN FORK 4: duress. RULED: (a), with a deletion seal.** The duress threat model hides everything
from the actor's device.
- (a) Record on duress too, with the clip sealed out of every listing until the alert is attended
  or 24 h pass (recommended): the evidence exists, and a coerced Owner's timeline shows nothing
  new.
- (b) Do not record on duress.

**Ruling: (a), plus a deletion seal.** The clip is recorded and sealed out of every listing until the
alert is attended or 24 h pass. A sealed duress clip cannot be deleted by anyone, the Owner included,
for 7 days from its creation. Whoever triggered duress may be the person being forced, so the attacker
must not be able to make them erase it. After the 7 days the normal retention rules apply. The seal
blocks deletion only; the listing rule is unchanged.

Under (a) the event carries `kind: duress` on this subject only, never in a frame any app reads.

**The consumer.** A durable pull consumer, `argus-recorder-guard`, on `ARGUS_GUARD`.
- Its filter subjects are `argus.guard.v1.episode` and `argus.guard.v1.safety_alert`. NATS 2.11
  takes several filters per consumer; if lib/nats keeps one filter per durable, it becomes two
  durables.
- Explicit ack, a 30 s ack wait and no delivery limit, because a lost trigger is a lost clip.

**Idempotency by event id.** `guard_event_inbox(event_id PRIMARY KEY, received_at)` in
`recorder.db` is written in the same transaction as the clip's state, and the message is acked after
the commit. A redelivery finds the row and acks.

**Order.** The clip's state is a monotonic window. `opened` and `touched` extend `endAt` to
`max(endAt, at + postRollS)`, `closed` does the same from its own `at`, and an event older than the
ring is recorded as a trigger with nothing left to keep. Out-of-order delivery therefore converges.

**A late trigger.** An event that arrives after its pre-roll has left the ring (NATS down for more
than five minutes) records the window as a gap with reason `trigger_late` and asks for the SD
backfill.

**Allow-list entries for track B** (the table beside `nats-subject.hxx`, user `recorder`):
- **Subscribe:**
  - `argus.guard.v1.episode` and `argus.guard.v1.safety_alert`, through the durable consumer;
  - `argus.camera.v1.change`: a camera added, removed or renamed, its record mode, and its "no
    audio", "no recording" and privacy flags;
  - `argus.settings.v1.module`: surveillance on or off, so the recorder idles and the purge reaches
    it;
  - `_INBOX.>` for replies.
- **Publish:**
  - `argus.recorder.v1.>`, the recorder's own subjects (ruled 2026-10-13); `argus.recorder.v1.change`
    carries its `ResourceChanged` notices;
  - on `ARGUS_GUARD`: `$JS.API.STREAM.INFO.ARGUS_GUARD`;
  - for its durable `argus-recorder-guard`, the consumer subjects:
    - `$JS.API.CONSUMER.CREATE.ARGUS_GUARD.argus-recorder-guard`, with its `.>` form;
    - `$JS.API.CONSUMER.INFO.ARGUS_GUARD.argus-recorder-guard`;
    - `$JS.API.CONSUMER.MSG.NEXT.ARGUS_GUARD.argus-recorder-guard`;
    - `$JS.ACK.ARGUS_GUARD.argus-recorder-guard.>`;
  - the same consumer subjects for its module-feed durable on `ARGUS_SETTINGS_MODULE`;
  - `$JS.API.STREAM.CREATE.ARGUS_RECORDER_CHANGE` and `$JS.API.STREAM.INFO.ARGUS_RECORDER_CHANGE`.
- **Guard** publishes the two new subjects. Its grant covers them if its entry is
  `argus.guard.v1.>`; otherwise the two names are added.
- **argus-sync** subscribes to `argus.recorder.v1.change` to fan the notices out.

## 6. HLS playback for the app's timeline

**Which service.**
- (a) argus-camera serves the playback routes as a thin gateway, and the recorder is the authority
  behind its internal gRPC (recommended).
  - argus-camera already has the app-facing TLS listener, the mDNS route announcements, the full
    filter chain, and the session feed that closes a revoked session's live view.
  - The recorder keeps the tokens and the access log, and argus-camera never opens `recorder.db`
    (rule 27).
  - It keeps every public HTTP surface out of the recording process, which is the separation the
    spec and the reviewer's survival argument ask for.
  - The costs: one loopback gRPC hop per segment (about 1 MB at 2 Mbps), and playback, not
    recording, pauses while argus-camera restarts.
- (b) The recorder serves its own HTTPS listener and announces `/recording`. That is one hop
  fewer, at the price of putting the app's HTTP surface inside the process that must not stop.

**The routes** (served by argus-camera; DeviceFilter → JwtFilter → RoleFilter on every one, recorded
in the route baseline):
- `GET /recording/clips?cameraId&from&to`: the timeline's clips and gaps, with no URLs
  (`ListClips`).
- `GET /recording/clips/{id}/playlist.m3u8`: mints the view and returns the playlist (`MintView`).
- `GET /recording/view/{token}/init/{n}.mp4` and `GET /recording/view/{token}/segment/{seq}.m4s`:
  the bytes, streamed by the recorder from RustFS (`ReadObject`). No bucket path, signed URL or
  credential ever reaches the app.
- The lock, export and delete routes come later, each with its own audit action.

**Authorisation.**
- Owner only in v1: rows in `role-access.hxx` (track A's file) and a new capability,
  `recordings.view`, in the surveillance module, Owner only, so the app and the assistant read the
  same answer.
- `kModuleRoutes` maps `recording` to surveillance, so turning the module off answers 403
  `MODULE_DISABLED`.

**Short-lived segment URLs.**
- `MintView` creates a view token: 256 random bits, stored as SHA-256, bound to the user, the
  session, the device hash and the clip.
- The token lives 10 minutes and is reusable inside that window, because scrubbing re-reads
  segments. Every URL in the playlist carries it.
- The segment routes still pass the full filter chain. ExoPlayer and hls.js send the
  `Authorization` header, and AVPlayer uses `?token=`, which JwtFilter already reads.
- A leaked URL alone reads nothing, and a JWT without the view token reads nothing. A revoked
  session's JWT is refused by the filter on the next segment.
- When the token expires mid-view, the player re-requests the playlist, which mints a new view.

**The access log.**
- One row per view session, written by `MintView`: `recording_access(id, user_id, session_id,
  device_hash, clip_id, action: view, range_start, range_end, viewed_from, viewed_to, at)`.
  `viewed_from` and `viewed_to` stay NULL until the session ends (ruling 5).
- It is server-side only, never synced, and kept 365 days.
- Download, export, lock and delete add their own rows when those routes land.

**DESIGN FORK 5: scrubs. RULED: (a), one row per view session, extended at session end.** The spec
lists scrub as an audited action, and this note's brief says one row per view.
- (a) One row per view in v1 (recommended): the server cannot tell a scrub from a seek-ahead buffer
  without client signals.
- (b) Coalesced scrub rows: one per view per minute with out-of-order segment reads, carrying the
  minimum and maximum range.

**Ruling: (a), extended at session end.** One access row per view session, never one per scrub event.
`viewed_from` and `viewed_to` hold the minimum and maximum of the range actually played, and they are
written when the session ends: when the view token expires, or when the app ends the view explicitly.
A session the server never sees end closes at its token's expiry, so no row stays open.

**DESIGN FORK 6: locked clips and the module purge. RULED: (a).**
- (a) The surveillance purge refuses while locked clips exist and names them, so the Owner unlocks
  them first (recommended: a lock is an explicit evidence decision).
- (b) The purge deletes locked clips too.

**Ruling: (a).** The module purge refuses while locked clips exist and names them, and the Owner unlocks
them first. An evidence lock outranks a module switch: turning the surveillance module off does not
release a locked clip.
