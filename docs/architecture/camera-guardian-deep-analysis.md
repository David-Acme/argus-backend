# Argus Camera Guardian: Deep Technical Analysis and Target Architecture

**Status:** Research and architecture proposal; no implementation included  
**Audit date:** 2026-09-12  
**Scope:** `argus-camera`, `argus-guard`, identity, VLM, LLM, STT, TTS, voice,
notifications, evidence storage, NATS transport, and camera action RPC  
**Deployment model:** Fully local and self-hosted, preserving the existing
microservice boundaries and shared SDK rules

## 1. Executive assessment

Argus already contains the difficult physical pieces of a local camera guardian:
live video ingestion, object detection, zones, face identification, local VLM and
LLM inference, speech recognition, speech synthesis, camera talk, alarm and siren
control, notifications, evidence upload, and health checks. The current system is
therefore more than a proof of concept. However, its orchestration does not yet
provide a trustworthy autonomous guardian.

The central problem is not model quality in isolation. It is the absence of one
durable, unambiguous encounter identity that binds the detected person, track,
best crop, face result, spoken exchange, risk assessment, notification, and every
physical action. Several current paths are keyed only by `cameraId`, while the
camera may see different or multiple people. That makes it possible to assess one
person with another person's crop or cached identity. No prompt improvement can
repair that architectural ambiguity.

The second critical problem is authority. The LLM assessment loop can directly
invoke alarm and siren tools before the service-level policy, action budget, and
`arm_siren` setting are applied. A generative model consuming hostile visual and
audio input must never hold that authority. The LLM should produce a constrained,
typed proposal. A deterministic policy enforcement point must be the only path to
physical effects.

The third problem is temporal behavior. The design document describes dwell
gating and a staged encounter, but the active deployment has all dwell thresholds
set to zero. It consequently evaluates and notifies on transient detections. The
database snapshot contains many repeated incidents and open encounters, while the
gateway simultaneously runs a second raw camera notification path. This produces
noise, duplicate user-facing behavior, unnecessary model load, and an experience
that feels reactive rather than observant.

The fourth problem is dialogue. The current sequence is a cached greeting, a fixed
five-second recording, and a canned acknowledgement before the LLM interprets the
reply. That is a script with AI appended to it, not a conversation. A natural
guardian needs endpointed listening, explicit turn state, grounded short replies,
repair behavior for unintelligible audio, and memory limited to the current
encounter. Natural language can be flexible; authority and state transitions must
remain deterministic.

The recommended design is a **hierarchical hybrid guardian**:

1. `argus-camera` emits immutable per-object observations with stable identifiers
   and evidence references.
2. `argus-guard` owns a persistent encounter aggregate and deterministic state
   machine.
3. VLM, STT, and LLM return observations or proposals, never physical commands.
4. One policy enforcement point validates risk, confidence, budgets, mode,
   configuration, freshness, and idempotency before executing an action.
5. Dialogue is bounded and natural, with VAD-driven turns and a safe scripted
   fallback.
6. Notifications, evidence, and memory consume the finalized encounter truth, not
   raw detection noise.

This architecture preserves Argus's local-first identity and existing service
ownership. It does not require a new general agent framework or a new speculative
microservice.

## 2. Method and evidence boundary

The analysis used four evidence classes:

- The project and service `AGENTS.md` rules, all relevant `CONTEXT.md` files, wire
  contracts, database schemas, service implementations, configuration examples,
  tests, and the existing camera automation plan.
- Read-only inspection of the active Docker deployment, non-secret runtime
  configuration, health state, guard and gateway logs, and a copied guard database
  snapshot.
- Execution of the existing test suites. `argus-guard` passed 12/12 tests and
  `argus-camera` passed 23/23 tests when local sockets were permitted. Four camera
  tests initially failed inside the filesystem/network sandbox because socket
  creation was denied; the same tests passed outside that sandbox.
- Current primary or authoritative external sources: ONVIF, NATS, NIST, SIA,
  llama.cpp documentation, the official model card, and research papers.

No camera audio was sent, no listen operation was initiated, no alarm or siren was
activated, and no runtime configuration or database was modified during this
audit.

There is visible source/deployment drift: active container log line numbers do not
match the current working-tree line numbers. Runtime evidence describes the active
images at the audit timestamp; source findings describe the current workspace.
They are correlated below but are not treated as an identical binary snapshot.

## 3. Current end-to-end flow

```text
Camera RTSP
  -> go2rtc / frame capture
  -> YOLO object detection
  -> camera-local IoU tracking, zones, dwell and aggregation
  -> face identify or auto-enroll
  -> object_detected over Core NATS
       |-> gateway raw CameraNotifier -> notification service
       `-> argus-guard
             -> deterministic initial danger
             -> encounter correlation
             -> cached greeting through TTS + camera talk
             -> fixed-duration camera recording -> STT
             -> canned acknowledgement
             -> latest camera crop -> VLM caption
             -> LLM JSON/tool loop
             -> incident, assessment and evidence records
             -> notification / announce / alarm / siren
```

The individual integrations work, but the arrows do not carry a common encounter,
observation, object, and evidence identity. The result is an orchestration whose
components can each succeed while the overall decision is about the wrong temporal
subject.

## 4. Runtime findings from the connected camera

At `2026-09-12T12:37:54-05:00`, both the camera and guard containers reported
`running` and `healthy`. The active configuration had object inference at 2 FPS,
6 FPS during activity, and 10 FPS bursts. Identity, clear-face capture,
auto-enrollment, actions, greeting, fixed five-second listening, reply, VLM/LLM
assessment, notification, alarm, and siren support were enabled.

The operational settings differ materially from the intended staged plan:

| Setting | Active value | Architectural effect |
|---|---:|---|
| Alert dwell | 0 ms | Immediate event |
| Monitor dwell | 0 ms | Immediate event |
| Night dwell | 0 ms | Immediate event |
| Motion gate | false | Detector runs continuously at configured cadence |
| Ignore static persons | false | Persistent people remain eligible |
| Guard mode | home | Unknown daytime person starts at medium |
| Notify level | medium | Immediate unknown event is user-notifiable |
| Action cooldown | 120 s | Limits some effects, not assessments or all paths |
| Max actions per hour | 4 | Not applied consistently to encounter actions |
| Siren | enabled, 20 s | A generative decision can reach a physical effect |

The copied guard database snapshot contained:

| Observation | Count |
|---|---:|
| Unknown, medium, `person_day` incidents | 62 |
| Known, none, `known_person` incidents | 26 |
| Unknown, high, `person_night` incidents | 26 |
| Known but still labeled `person_day` | 12 |
| Notifications marked sent | 48 |
| Greetings marked sent | 29 |
| Fixed listens marked sent | 29 |
| Canned replies marked sent | 13 |
| Announcements sent / failed | 10 / 15 |
| VLM tool observations logged | 18 |
| Open escalating encounters | 24 |
| Open assessing encounters | 22 |

All known incidents in this snapshot had `personId=0`. Five new identity records
were auto-enrolled during the observed runtime window. This is consistent with
track or cache fragmentation and makes automatic enrollment unsuitable as an
unattended identity truth source in its present form.

The active guard logs showed VLM durations from roughly 1.1 to 10.5 seconds and LLM
durations from roughly 4.1 to 19.7 seconds. Combined assessments commonly took
8–16 seconds and sometimes approached 30 seconds. Several LLM turns were malformed
JSON, with bracket and quote errors. No observed assessment used a meaningful tool
round; the VLM had already run before the LLM turn.

The gateway logs showed raw camera notifications being delivered before the guard
completed assessment, followed by many raw events suppressed by the gateway's
budget. Independently, the guard delivered its own notifications. This confirms
two competing user-notification authorities.

## 5. Prioritized findings

### 5.1 P0: the model can bypass the physical-action policy

`GuardAssessment::assess()` executes `camera.alarm` and `camera.siren` tool calls
directly (`guard-assessment.cc`, approximately lines 354–393). This happens before
`GuardService` evaluates the action threshold, checks the encounter cooldown,
persists the incident, or applies `config_.armSiren` (`guard-service.cc`,
approximately lines 422–535).

This creates two policy paths:

- Service path: danger threshold + cooldown + `armSiren` + action RPC.
- Agent-tool path: model output + camera RPC.

The second path is a safety bypass. It remains a bypass even if the prompt tells
the model to be cautious. Visual text, spoken audio, ASR mistakes, model
hallucination, malformed context, and prompt injection are all untrusted inputs.
Research has demonstrated indirect instruction injection through both images and
sounds.[^8] NIST treats prompt injection and indirect prompt injection as integrity,
availability, privacy, and misuse risks for generative systems.[^7]

**Required outcome:** the assessment layer must lose all effectful camera tools.
It may request more perception through a narrowly typed read-only interface, but
alarm, siren, notification, PTZ, and announce execution must be possible only
after a deterministic authorization decision.

### 5.2 P0: evidence is not bound to the detected object

The camera stores one latest full frame and one latest person crop per camera in
`SnapshotStore` (`snapshot-store.cc`, lines 16–49). `PersonCropRequest` contains
only `camera_id`; the RPC returns the latest crop or even the latest full frame
(`actions.proto`, lines 32–41; `camera-action-rpc-service.cc`, approximately lines
277–304). The guard receives `captured_at` but does not validate it against the
event.

If another person crosses the scene, if an older crop remains, or if the VLM request
is delayed, the assessment can use evidence unrelated to the incident. This is the
highest-integrity defect in the perception chain.

**Required outcome:** every observation must include an immutable `observationId`,
`trackId`, `objectId`, `capturedAt`, and an evidence handle or content hash. The
guard must request exactly that evidence and reject stale or mismatched content.

### 5.3 P0: identity state can bleed between people

`IdentityKnownPersonMatcher` caches by `cameraId`, not track or encounter. A cached
known result is returned immediately during the best-shot window
(`identity-known-person-matcher.cc`, lines 59–76). A different person entering the
same camera can inherit that result. Unknown/best-shot state has the same temporal
ambiguity.

Additionally, the guard parser marks a known object but does not copy its
`personId`; it only copies `personId` in the non-known branch (`guard-policy.cc`,
lines 53–72). This explains known incidents with `personId=0`, prevents the known
visitor greeting, and prevents closing unknown encounters when a person becomes
known.

**Required outcome:** identity evidence belongs to `(cameraId, trackId,
observationId)`, and a stable `personId` is propagated for both known and enrolled
unknown people. Identity should use temporal hysteresis and confidence history,
not a single binary result.

### 5.4 P0: multiple people collapse into one class entry

Pending event aggregation deduplicates by object class name
(`camera-operator-service.cc`, approximately lines 547–575). Two or more people
therefore become one `person` object. The tracker selects only the largest primary
person for top-level dwell and track fields. The greedy association also does not
reserve a track once matched within a frame, so two detections can select the same
track.

This prevents reliable group count, companion reasoning, per-person identity,
per-person dialogue, and risk escalation. Multi-object tracking exists specifically
to retain object identities across video frames; ByteTrack demonstrates the value
of associating even lower-confidence detections to reduce missed and fragmented
tracks.[^9]

**Required outcome:** events must carry one object record per physical detection,
each with its own track and evidence. No person-class deduplication is allowed.

### 5.5 P0: encounter state violates its own schema

The schema permits `observing`, `assessing`, `escalating`, and `closed`. Current
source writes `observed` for an observation-only transition
(`guard-service.cc`, lines 408–417). Replaying that insert/update against a fresh
schema produces a SQLite `CHECK constraint failed` error.

**Required outcome:** use one `enum class` at the database boundary, fix the state
name, and add a schema-contract test that exercises every transition against a
fresh database.

### 5.6 P0: physical effects lack durable idempotency and recovery

Action RPC requests contain a target and arguments but no `commandId`,
`encounterId`, expected policy version, lease, or idempotency key. Effects are
logged after execution. A crash between execution and logging can cause an
unrecorded effect; a retry can duplicate it.

Siren auto-disable is an in-memory timer. If guard or the host restarts after arm
and before the callback, there is no durable lease that guarantees disarm. All
camera RPCs also share a 60-second client deadline, including quick control
operations.

**Required outcome:** use a persistent action command with a unique ID and an
outbox/inbox pattern. The camera must deduplicate commands. Siren activation must
be a persisted lease with an expiry enforced by both guard and camera, including
startup reconciliation. Deadlines must be action-specific.

### 5.7 P1: the deployed system bypasses its intended dwell architecture

The automation plan specifies approximately 3 seconds for alert zones, 12 seconds
for monitor zones, and 8 seconds at night. The active deployment sets all three to
zero. Home-mode unknown detections therefore become medium immediately and meet
the notification threshold without demonstrating dwell or suspicious behavior.

The appropriate behavior is not one global delay. Entry into a protected alert
zone can be immediate, while a porch, sidewalk, or monitor zone should require
trajectory, dwell, return, or challenge evidence. Thresholds must be zone-specific
and tuned from replay data.

### 5.8 P1: model threat does not become system risk

The LLM result can lower a soft case through `veto`, and its action booleans can
trigger effects, but `assessment.threat` is not merged upward into `danger`. A
valid `high` or `critical` assessment with all action booleans false can leave the
incident at medium. This makes the semantic threat label largely descriptive.

**Required outcome:** model output becomes one evidence signal in a deterministic
risk function. It can raise a soft score only when supported by explicit observable
tags and confidence; it cannot lower hard floors. Action selection is derived from
the final risk and policy, never independent booleans.

### 5.9 P1: the dialogue is ordered incorrectly

The current sequence marks `greeted=true` even when the camera rejects the
announcement. It then records a fixed-duration window, labels it successful if
audio capture succeeded rather than if speech was understood, and emits a canned
reply before the LLM reads the transcript (`guard-service.cc`, approximately lines
139–242).

Voice activity detection is already an Argus capability, but the camera path does
not use it. Spoken dialogue literature identifies VAD/end-silence handling as a
core turn-boundary mechanism and acoustic echo cancellation as necessary for
reliable duplex interruption.[^10] Silero VAD already provides local ONNX/C++
integration patterns consistent with this repository.[^11]

**Required outcome:** acknowledgement follows understanding. Listening ends on
speech endpoint or bounded timeout, not a fixed five seconds. The first version can
remain half-duplex, but it needs explicit `Speaking`, `Listening`, `Thinking`, and
`Repairing` states. Full barge-in should only be enabled after echo cancellation is
measured on each supported camera.

### 5.10 P1: unconstrained JSON wastes latency and loses decisions

The system prompt requests exact JSON, but the active model repeatedly produced
unbalanced brackets and quotes. The parser then abandons the turn. llama.cpp
supports GBNF and JSON-schema-constrained generation; its documentation explicitly
describes converting JSON Schema to a sampling grammar.[^5]

**Required outcome:** use grammar-constrained output with `additionalProperties:
false`, required fields, enumerated values, bounded lengths, and a protocol version.
The proposal still requires semantic validation, but syntax failure should approach
zero. Unknown tool names must be rejected, not treated as valid tool calls.

### 5.11 P1: retained events are consumed ephemerally

The camera creates a seven-day JetStream stream, but its normal publisher uses Core
NATS and guard subscribes with a Core NATS ephemeral subscription. Retention
therefore helps inspection, not guard recovery. Events emitted while guard is down
are not replayed. NATS documents durable consumers with explicit acknowledgements,
and JetStream streams provide file retention and message-ID duplicate suppression
within a configured window.[^3][^4]

**Required outcome:** guard consumes a durable JetStream subject with explicit ack
after the encounter transition and outbox record commit. Events and commands carry
stable message IDs; handlers remain idempotent because at-least-once delivery can
redeliver.

### 5.12 P1: two systems own camera notifications

The gateway notifies from raw `object_detected` events while guard notifies after
assessment. They have independent budgets and semantics. The user can receive an
unverified notification followed by a guard notification, or the raw route can
consume its budget before a more meaningful incident.

**Required outcome:** guard is the only normal authority for incident
notifications. The gateway raw notifier becomes either a pure transport adapter or
a clearly defined degraded fallback activated only when guard readiness is absent.
One encounter produces one evolving notification thread, with critical transitions
allowed to update or bypass ordinary aggregation.

### 5.13 P1: expected-guest status is global

`activeGuest(now)` returns any active time window. It is not scoped to camera,
entrance, person, host, delivery, or a one-time credential. During any active guest
window, every unknown person at every camera can be lowered to low.

**Required outcome:** guest expectations need site/entrance scope, time window,
host, optional appearance/name hints treated only as weak evidence, and a one-time
challenge or resident approval. Merely having some expected guest must never clear
an unrelated person.

### 5.14 P1: privacy controls are incomplete

Raw event JSON, VLM captions, summaries, tags, and stranger transcripts are stored
without an evident deletion workflow. Camera and guard evidence are uploaded under
timestamp paths with no manifest-level retention or deletion binding. The
`greet_listen` action stores up to 300 characters of raw stranger speech.

The project correctly keeps stranger dialogue out of long-term assistant memory,
but the incident store itself still needs minimization. The NIST Privacy Framework
recommends identifying and managing privacy risk as a product property, not only
as a security control.[^12]

**Required outcome:** continuous audio remains an ephemeral ring buffer; silence is
discarded; transcripts are stored only when needed for an incident and under a
severity-based retention policy. Evidence must have a manifest, expiry, access
audit, encryption at rest, and one deletion path covering database rows and object
storage. Memory receives a finalized, redacted encounter summary, never raw speech
or raw detection events.

### 5.15 P1: service-to-service authorization is too coarse

The shared SDK creates insecure gRPC channels (`grpc-client-base.cc`, lines 15–27)
and camera actions use the same fleet secret metadata for every call. A local
network boundary is not an identity boundary. NIST zero-trust guidance explicitly
rejects implicit trust based only on network location and recommends granular
application/service identities.[^13]

The guard's direct HTTP routes are also manually registered without the project's
normal DTO, filter, role-access, and response conventions. They are bound to
loopback in Compose, but a local process could still target them directly.

**Required outcome:** authenticated service identity, preferably the existing PKI
with mTLS, plus method-scoped authorization. Camera effect RPCs accept only the
guard executor identity. Administrative guard HTTP operations must pass through
the established gateway/auth/filter architecture.

### 5.16 P2: code structure and lifecycle violate project rules

`GuardService` creates `GuardRepository` and `S3StorageService` locally rather than
holding injected private dependencies. `EvidenceUploader` is a singleton and also
creates storage locally. Guard endpoints perform parsing, validation, persistence,
and response creation inside raw handlers. These violate the repository's manual
DI, controller thinness, validation, and response rules.

These are not cosmetic findings. Explicit dependency ownership is necessary to
test failure modes and guarantee one policy path.

### 5.17 P2: current tests prove components, not guardian behavior

Passing unit suites do not cover the most important temporal and safety properties.
The tests do not establish:

- crop/event/track identity binding;
- two people in one frame and identity-switch resistance;
- correct propagation of a known `personId`;
- every encounter transition against the real schema;
- concurrent duplicate delivery and idempotent actions;
- a restart while the siren lease is active;
- hourly and per-encounter budgets across every action path;
- VLM/LLM prompt injection through signs, clothing, screens, and audio;
- replay/photo attacks against face identity;
- dialogue endpointing, echo, silence, noise, interruption, and ASR failure;
- notification consolidation;
- end-to-end latency and load under repeated activity.

The opt-in “live” VLM tests return successfully when environment variables are
absent, so a normal green CTest run does not imply that live VLM/LLM inference ran.

## 6. External research implications

### 6.1 Prefer standard camera capabilities behind the existing driver boundary

ONVIF Profile T standardizes advanced video streaming, motion/tamper events,
metadata, HTTPS-related capabilities, PTZ, relay outputs, and conditional
bidirectional audio.[^1] Profile M provides generic object, face/body, event, and
analytics metadata interfaces.[^2] Argus should keep vendor-specific Tapo support,
but capability discovery should evolve behind `ICameraDriver` toward these
standards. This avoids baking one camera's talk protocol into guard behavior.

This is a future compatibility direction, not a reason to add an unused ONVIF
abstraction immediately. Add capabilities only when a conformant camera becomes a
current consumer, consistent with the repository's no-speculative-structure rule.

### 6.2 Face identity is evidence, not authorization

NIST evaluation shows presentation-attack detection accuracy varies widely by
attack type, and that some attacks remain difficult; video sequences often perform
better than single stills.[^6] A face match from one 2D crop must not silently turn
an encounter into “safe.”

Use identity as a confidence-bearing temporal observation. Important clearances
should require corroboration: consistent identity across frames, anti-spoof/PAD
signal where available, expected route or schedule, device/credential confirmation,
or resident approval. Unknown auto-enrollment should create a candidate person
cluster, not a trusted identity.

### 6.3 A VLM viewing a hostile scene crosses a trust boundary

An intruder controls what the camera sees and hears. Text on a shirt, phone, sign,
or played audio can be designed to influence a multimodal model. The VLM prompt
must state that scene text and speech are observations, never instructions, but
prompt text alone is not a sufficient defense.[^7][^8]

Architectural isolation is the defense: perception returns a typed description;
the deterministic risk engine consumes bounded fields; physical permissions are
not present in the model process. OCR can be separated into an explicitly untrusted
field. Adversarial fixtures must be part of the release gate.

### 6.4 False-alarm reduction is a first-class security property

The Security Industry Association's CP-01 work explicitly targets false-alarm
reduction and informative signal handling.[^14] A guardian that alarms too often
teaches residents to ignore it and exhausts notification budgets. Precision is not
only a UX metric; it preserves response credibility.

The correct cascade is cheap detection, stable tracking, zone/dwell qualification,
identity and expectation correlation, then expensive semantic assessment. The
current zero-dwell deployment reverses the intended economics.

### 6.5 Production evaluation must match deployment conditions

The NIST AI RMF calls for documented roles, human oversight, test sets, deployment-
representative evaluation, production monitoring, and measured failure response.[^15]
For Argus this means private replay data from the actual camera angle, day/night,
weather, echo, distance, languages, occlusion, multiple people, and resident/intruder
roleplay—not only generic model benchmarks.

The official LFM2.5-VL-450M card describes a compact general-purpose edge VLM with
a 350M language backbone and an 86M vision encoder.[^16] That footprint is valuable
for local latency, but its general-purpose nature is another reason not to treat a
single caption as a calibrated threat detector. Argus must measure this exact
pinned artifact and quantization on its own surveillance tasks.

## 7. Alternatives considered

| Alternative | Reliability | Naturalness | Safety | Local cost | Verdict |
|---|---|---|---|---|---|
| Rules and fixed phrases only | High for known cases | Low | High | Low | Keep as degraded fallback |
| Full LLM agent with physical tools | Low/variable | Potentially high | Unacceptable | High/variable | Reject |
| Hierarchical hybrid: persistent state + deterministic policy + generative dialogue | High when engineered | High | High | Controlled | **Recommended** |

The rules-only design cannot interpret unusual visitors and sounds robotic. The
full-agent design appears autonomous but combines nondeterministic planning with
physical authority and adversarial inputs. The hybrid gives the model freedom over
language and semantic hypotheses while retaining deterministic ownership of time,
identity, risk, and effects.

## 8. Target architecture

### 8.1 Preserve current domain ownership

```text
argus-camera
  Owns: camera credentials, streams, frames, detector, per-camera tracking,
        immutable evidence capture, driver capabilities, talk/alarm/siren execution
  Does not own: threat policy, user notification, conversation decisions

argus-guard
  Owns: encounter aggregate, risk policy, dialogue state, action authorization,
        action outbox, incident lifecycle, finalized encounter summary
  Does not own: camera database, identity database, model internals

argus-identity
  Owns: person candidates, embeddings, known/trusted status, identity observations

argus-vlm / argus-stt / argus-llm / argus-tts
  Own: bounded inference operations; return typed results through shared SDKs

argus-notification
  Owns: persisted delivery; receives one incident-centric command from guard

argus-memory
  Receives: finalized and redacted system encounter summaries only
```

This fits the existing microservice isolation and shared SDK model. Guard never
opens another service's database.

### 8.2 Observation contract v2

The current class-level event should evolve additively into an immutable,
versioned observation envelope. Conceptual fields:

| Area | Required fields |
|---|---|
| Envelope | `schemaVersion`, `eventId`, `observationId`, `cameraId`, `capturedAt`, `publishedAt` |
| Object | `objectId`, `trackId`, class enum, detector confidence, bounding box, zone ID/type |
| Temporal | first/last seen, dwell, trajectory summary, track confidence, occlusion state |
| Identity | `personId?`, identity state enum, match confidence, quality, observation time, model version, PAD state |
| Evidence | evidence capability/ref, crop hash, frame hash, width/height, freshness expiry |
| Environment | guard-relevant mode hints only; no private secrets or full user records |
| Integrity | producer service identity and trace/correlation ID |

Each person is a separate object. Top-level `trackId` and class-based deduplication
are removed from the semantic contract. A legacy adapter can publish the old shape
during a frontend/backend contract transition, but the encounter engine should
consume only the new canonical form.

### 8.3 Encounter aggregate

The encounter is the unit of autonomy. It persists:

- stable encounter ID and revision;
- participating tracks/cameras/person candidates;
- first/last seen and state transition timestamps;
- best evidence per purpose: identity, behavior, and audit;
- identity hypothesis history, never only the latest value;
- deterministic risk inputs and resulting risk revision;
- current dialogue state and bounded transcript summary;
- authorized, pending, completed, failed, and compensated actions;
- notification thread ID;
- closure reason and retention class.

Updates use optimistic revision checks or one serialized encounter command path.
Every incoming observation is deduplicated by `eventId` and applied transactionally
with an outbox record.

### 8.4 State machine

| State | Purpose | Allowed exits |
|---|---|---|
| `Observing` | Establish a stable track and dwell | `Verifying`, `Closed`, `Degraded` |
| `Verifying` | Bind evidence, identity, zone, expectations | `Challenging`, `Resolved`, `Escalating`, `Degraded` |
| `Challenging` | Deliver one short contextual utterance | `Listening`, `Escalating`, `Degraded` |
| `Listening` | VAD/timeout-bounded stranger turn | `Interpreting`, `Challenging`, `Escalating`, `Degraded` |
| `Interpreting` | STT + typed semantic proposal | `Challenging`, `Resolved`, `Escalating`, `Degraded` |
| `Resolved` | Known/expected/benign outcome | `Closed`, `Observing` on meaningful new evidence |
| `Escalating` | Notify/announce/alarm/siren under policy | `Resolved`, `Closed`, `Degraded` |
| `Degraded` | A required dependency or sensor is unreliable | `Observing`, `Escalating`, `Closed` under fail-safe policy |
| `Closed` | Terminal summary and retention assignment | none |

State names must be shared enums at DB boundaries. Transitions are commands with a
reason code, previous revision, and timestamp. A periodic sweeper closes stale
encounters and reconciles pending leases.

### 8.5 Deterministic risk engine

Risk should be a structured result, not a prompt-derived adjective:

```text
Risk = hard floors
     + temporal/zone behavior
     + identity and expectation confidence
     + multi-person/context signals
     + bounded semantic evidence
     + system-health uncertainty
```

Hard floors include protected-zone entry while away, forced-entry sensor events,
verified weapon-like object with temporal corroboration, camera tampering during an
active encounter, explicit aggression after a challenge, and repeated route-aware
re-entry. A model can never veto a hard floor.

Semantic evidence must name observable reasons such as `concealed_face`,
`carrying_box`, `attempting_door`, `raised_object`, `calm_delivery_reply`, or
`unintelligible_reply`, each with confidence. The risk engine decides whether that
evidence is strong enough to raise a tier. A caption alone does not.

Suggested action policy:

| Risk | Default behavior |
|---|---|
| None | Close or silently retain minimal summary |
| Low | Observe; optionally greet at an entrance once |
| Medium | Challenge, listen, create/update one notification |
| High | Firm announcement, immediate notification, alarm if corroborated |
| Critical | Immediate alert; alarm; siren only with explicit owner policy and corroborated hard floor |

Siren should default off in examples and new installations. Enabling it should be
separate from enabling benign camera speech.

### 8.6 Model boundary

The LLM request should contain a small typed encounter view, not concatenated raw
strings. It may return:

- `intent`: delivery, visitor, resident request, refusal, threat, unknown;
- `dialogueAct`: greet, clarify, request verification, instruct leave, warn, none;
- `utterance`: short text within policy constraints;
- observable semantic tags with confidence;
- a concise rationale for audit;
- `needsMoreEvidence`: a read-only perception request selected from an allowlist.

It must not return or invoke `siren`, `alarm`, notification, PTZ, user mutation,
memory writes, or arbitrary tool names. Output is grammar-constrained and then
validated. Invalid or timed-out proposals select a deterministic fallback.

VLM scene text and STT stranger speech are explicitly marked as untrusted quoted
data. They are never concatenated into the system-instruction channel. The model
process should not possess action credentials.

### 8.7 Natural guardian dialogue

Naturalness comes from continuity and relevance, not from longer speech.

1. **Observe before speaking.** Do not greet every transient pedestrian. Speak
   once when a stable person reaches an entrance or protected zone.
2. **Acknowledge only after understanding.** Replace the canned “Perfecto…” with a
   generated or deterministic response grounded in the transcript.
3. **Use short turns.** One idea and at most one question per utterance.
4. **Carry encounter context.** Do not repeat the greeting. Refer to the person's
   answer and the current verification step.
5. **Repair naturally.** On low ASR confidence: “No te escuché bien. ¿Puedes
   repetirlo?” After a second failure, stop looping and notify or close according
   to risk.
6. **Escalate tone, not personality.** Neutral -> direct -> firm. No insults,
   threats, fabricated police dispatch, or claims that reveal occupancy.
7. **Do not leak security state.** Never reveal resident names to unknown people,
   blind spots, whether anyone is home, recognition confidence, or response times.
8. **Disclose identity consistently.** Recommended default: “Hola, soy Argus. ¿En
   qué puedo ayudarte?” This feels intentional without pretending to be a human.
9. **Bound the exchange.** At most three meaningful stranger turns or 60 seconds,
   with lower limits for high-risk cases.

The audio controller needs a per-camera session lock, pre-speech buffer, VAD start
and end thresholds, noise floor adaptation, maximum utterance duration, and a
post-playback guard interval. Half-duplex is acceptable first. Barge-in becomes a
later hardware-qualified capability because speaker echo differs across cameras.

### 8.8 Action authorization and execution

All effects pass through one conceptual call:

```text
authorize(ActionProposal, EncounterSnapshot, PolicySnapshot)
  -> Denied(reason)
  -> Authorized(ActionCommand)
```

`ActionCommand` contains `commandId`, `encounterId`, `incidentId`, `cameraId`,
action enum, bounded parameters, policy revision, risk revision, creation time,
expiry, and idempotency key. Camera verifies caller identity, supported capability,
expiry, bounds, and duplicate status before execution.

The database transition, authorization audit, and outbox insert occur in one
transaction. Completion is recorded from an acknowledgement. Retries reuse the
same command ID. Siren is represented as `SetSirenLease(expiresAt)` rather than an
unbounded boolean. Camera independently disarms an expired lease.

### 8.9 Notification model

One encounter owns one notification thread:

- initial medium transition creates it;
- identity, reply, and risk changes update it;
- a high/critical transition can produce an urgent update;
- resolution closes it with a concise outcome;
- raw detections never directly notify in normal mode.

The body should describe what the system knows and its uncertainty, for example:
“Persona no reconocida en la entrada durante 18 s; respondió que trae un paquete.”
Avoid model adjectives such as “suspicious” without observable reasons.

### 8.10 Evidence and memory

Camera captures evidence once per meaningful encounter revision, not once per raw
event. The evidence manifest binds hashes, capture time, track/object IDs, model
versions, and retention class. Guard stores references, never bucket credentials
or private paths in sync data.

Suggested default retention policy, subject to owner choice and local law:

| Data | Default |
|---|---|
| Silence/non-incident audio | immediate discard |
| Benign closed encounter summary | 24 hours |
| Low/medium evidence | 7 days |
| High/critical evidence | 30 days |
| Audit of physical effects | longer, metadata only |
| Face embeddings/candidates | explicit identity lifecycle, not incident retention |

Long-term memory receives only a redacted finalized event such as “An expected
delivery arrived at the front entrance,” and only when camera observation memory is
enabled. It must not learn raw object events, stranger transcripts, or model
captions as facts.

### 8.11 Observability and degraded behavior

Keep `/health` cheap and nonblocking, as project rules require, but expose internal
readiness/metrics for:

- last frame age and capture failures per camera;
- actual detector FPS, queue delay, track count and identity switches;
- event publish/consume lag and redelivery;
- encounter count and age by state;
- evidence freshness and mismatch rejection;
- VLM/LLM/STT/TTS latency, timeout and invalid-output rate;
- dialogue starts, speech-detected rate, ASR confidence and repair rate;
- action authorization denials, duplicates, failures and active leases;
- notification create/update failures;
- auto-enrollment candidate rate;
- disk/object-store retention backlog.

Degraded policy must be explicit. Examples: if VLM is down, use tracking + zone +
identity + deterministic speech; if STT is down, do not pretend to understand;
offer a one-line fallback and notify when required; if guard is down, the gateway
may issue one degraded raw alert for protected-zone hard signals; if identity is
down, treat identity as unknown, not known.

## 9. Performance objectives

These are proposed product SLOs to validate on the user's actual hardware, not
claims about current performance:

| Metric | Target |
|---|---:|
| Protected-zone observation to cached first speech, p95 | <= 2.0 s |
| Speech end to transcript, p95 | <= 1.5 s |
| Transcript to short dialogue reply start, p95 | <= 3.0 s |
| Full semantic assessment, p95 | <= 10 s |
| Syntactically valid model proposals | >= 99.9% |
| Observation/evidence mismatch accepted | 0 |
| Duplicate physical effects for one command ID | 0 |
| Siren remaining armed beyond lease + recovery margin | 0 |
| Known-person identity switch within a stable track | measured and release-gated |
| Duplicate user notifications per encounter transition | 0 |

Fast speech and full assessment should run on separate lanes. A cached safe opener
must not wait behind VLM/LLM inference. Conversely, a slow model must not delay a
deterministic hard-floor alert.

## 10. Implementation backlog for DeepSeek v4.1 Flash

The following order minimizes risk and avoids building natural dialogue on top of
ambiguous perception.

### Phase 0: close immediate safety gaps

1. Remove effectful action tools from `GuardAssessment`; keep perception read-only.
2. Make `arm_siren=false` the code and example default.
3. Route every announce/alarm/siren through one guard action authorizer.
4. Fix `observed` versus `observing` with a shared enum and transition test.
5. Propagate known `personId` in the event parser.
6. Mark greeting successful only on accepted camera acknowledgement.
7. Disable the duplicate gateway raw notifier in normal guard-ready operation.

**Acceptance gate:** no model output can cause a physical effect without a logged
policy authorization; all schemas/tests pass; current functionality has a safe
fallback.

### Phase 1: make observation identity correct

1. Introduce additive observation/evidence contract v2 in the shared contracts SDK.
2. Carry one record per person with object and track IDs.
3. Key best-shot and identity caches by track, not camera.
4. Bind crop retrieval to observation/track and validate capture freshness/hash.
5. Replace or harden greedy tracking with a real per-frame assignment that cannot
   reuse one track; evaluate ByteTrack if its measured CPU cost fits.
6. Convert automatic enrollment to candidate clustering with deduplication and
   explicit promotion to known/trusted status.

**Acceptance gate:** multi-person replay proves no identity/crop crossover; every
assessment references the exact evidence it consumed.

### Phase 2: persistent encounter engine

1. Define encounter/state/action enums in the appropriate shared enum boundary.
2. Add additive guard schema for encounter revisions, observation links, transition
   history, inbox deduplication, and action outbox.
3. Consume the camera stream through a durable acknowledged JetStream consumer.
4. Make observation application and outbox creation transactional.
5. Add stale encounter closure and startup reconciliation.

**Acceptance gate:** kill/restart tests at every transition converge to one
encounter without lost observations or duplicate commands.

### Phase 3: action command safety

1. Version the camera action contract with command IDs, expiry and encounter IDs.
2. Add camera-side idempotency inbox and bounded action-specific deadlines.
3. Replace siren boolean arm with a persistent expiring lease and startup disarm.
4. Add service identity/mTLS and method-scoped caller authorization.
5. Persist authorization before execution and result after acknowledgement.

**Acceptance gate:** repeated delivery executes once; process and host restarts
cannot leave the siren beyond its lease.

### Phase 4: risk and guest policy

1. Separate hard-floor evaluation, evidence scoring, and action mapping.
2. Merge validated semantic evidence into risk; remove independent action booleans.
3. Scope expected guests to entrance/site/host/window and add one-time verification
   or resident approval.
4. Implement mode-specific behavior, including a real `Armed` policy.
5. Restore zone-specific dwell defaults and tune through replay/shadow metrics.

**Acceptance gate:** a documented scenario matrix deterministically maps the same
evidence to the same risk/action, independent of wording generated by the LLM.

### Phase 5: conversational controller

1. Add explicit half-duplex dialogue states per encounter and camera session.
2. Reuse the existing local VAD through its service/package boundary for endpointed
   capture; retain the STT SDK path.
3. Replace the canned pre-understanding reply with a grounded dialogue proposal.
4. Apply grammar-constrained typed LLM output and deterministic repair/fallback.
5. Add ASR confidence/empty handling, two-attempt repair, turn and time budgets.
6. Add echo measurement fixtures before considering barge-in.

**Acceptance gate:** delivery, neighbor, lost visitor, silence, noise, refusal, and
aggression roleplays produce short non-repeating grounded exchanges within budget.

### Phase 6: notification, evidence and memory convergence

1. Make guard the normal single notification authority and use encounter updates.
2. Create evidence manifests and a retention/deletion worker covering SQLite and
   S3 objects.
3. Store minimized/redacted transcripts by severity; audit access.
4. Feed memory only finalized redacted encounter summaries.
5. Add owner-visible controls for speech, listening, alarm, siren and retention as
   separate permissions/settings.

**Acceptance gate:** one encounter creates one coherent notification history and
all expired private evidence is demonstrably removed.

### Phase 7: production evaluation

1. Build a private replay corpus from the installed camera angle across day/night,
   weather, occlusion and multiple people.
2. Label tracks, identities, zones, expected outcomes, speech endpoints and actions.
3. Run shadow mode before each action tier: observe -> notify -> speak -> alarm ->
   siren.
4. Add adversarial images/signs/screens/audio, photo replay, masks, prompt
   injection, dependency timeouts, message redelivery and restart fault injection.
5. Add hardware-in-loop tests for talk, microphone, alarm and siren leases.
6. Gate releases on the SLOs and scenario matrix, not only CTest pass count.

## 11. Required test matrix

| Dimension | Minimum cases |
|---|---|
| People | zero, one, two crossing, group, occlusion, person leaves/re-enters |
| Identity | known, unknown, candidate, false match, identity switch, photo/screen replay |
| Zones | exclude, monitor transient, monitor dwell, alert entry, boundary jitter |
| Time/mode | home, away, night, armed, clock change, stale mode |
| Guest | correct entrance/window, wrong entrance, expired, unrelated stranger, reused challenge |
| Dialogue | delivery, neighbor, resident, silence, noise, low ASR, language switch, refusal, aggression |
| Adversarial | visual instructions, spoken instructions, misleading uniform, hidden face, bright light |
| Dependencies | detector/VLM/LLM/STT/TTS/identity/notification/storage/NATS unavailable or slow |
| Messaging | duplicate, reorder, delayed, lost connection, redelivery after restart |
| Effects | duplicate command, timeout after execution, camera rejection, restart during siren lease |
| Privacy | retention expiry, deletion, access audit, no stranger-memory capture |
| Load | static person for hours, rapid entries, multiple cameras, constrained CPU |

Every scenario should assert state transitions, final risk, authorized actions,
forbidden actions, notification count, evidence identity, and retained data—not
only the final HTTP status.

## 12. AGENTS.md implementation constraints

The implementation model must preserve the repository rules during every phase:

- use `enum class` for all constrained state/action/risk columns and convert only
  at DB boundaries;
- use parameter structs and designated initializers for all functions with three
  or more parameters;
- keep repositories in their feature/domain module and use async `DbClient` or the
  service's existing SQLite abstraction as appropriate;
- inject repositories, SDK clients, storage, policy, and clock dependencies as
  private members with `_` suffix; do not instantiate them inside request/event
  paths;
- keep controllers thin and use the standard filters, DTO validation and
  `AppConfig` responses;
- never open another service's database; cross-domain calls use shared typed SDKs;
- run heavy AI only through async variants or `BlockingTask`, never on event loops;
- size threads through `ThreadBudget`, never constants;
- preserve local-only inference and do not expose secrets, object paths, raw
  portraits or stranger data in logs/sync;
- use one module folder and its own `CMakeLists.txt`; consumers link by module;
- extend schemas additively and never reset a user's database as a feature side
  effect;
- use short function/class comments only; put design rationale in this document;
- keep the frontend's persisted-local-first and granular sync semantics intact;
- remove replaced paths in the same change, especially the duplicate notification
  authority and direct model action path.

Each phase should be a separate reviewable change with tests, contract coordination,
and no unrelated cleanup.

## 13. Product decisions and recommended defaults

These choices should be made explicitly before enabling higher action tiers:

| Decision | Recommended default |
|---|---|
| Guardian self-identification | “Soy Argus”; do not impersonate a human |
| Unknown auto-enrollment | Candidate cluster only; never trusted automatically |
| Benign entrance greeting | Enabled after stable entrance track; once per encounter |
| Continuous camera listening | Disabled; listen only during an active bounded exchange |
| Siren | Disabled until owner opt-in and hardware-in-loop lease tests pass |
| Expected guest clearance | Scoped + one-time verification or owner approval |
| Raw stranger transcript retention | Off for benign encounters; short severity-based retention otherwise |
| Memory of stranger speech | Never |
| Model authority | Perception/dialogue proposal only; no physical tools |
| Raw notifier | Degraded fallback only |

## 14. Definition of “autonomous guardian” for Argus

Argus should be considered autonomous only when it can:

- maintain the same person's encounter through occlusion, multiple frames and
  multiple cameras with explicit uncertainty;
- decide when silence is better than speech;
- speak once, listen to the actual reply, and respond to that reply without
  repeating a script;
- distinguish observation, interpretation, policy and execution;
- explain every escalation using persisted observable evidence;
- recover safely from model, network, process and host failures;
- prevent a model or intruder-controlled input from bypassing physical policy;
- minimize and expire private evidence;
- remain useful in degraded mode without pretending unavailable capabilities work;
- prove these properties through replay, adversarial, restart and hardware tests.

That is the difference between a bot connected to a camera and a guardian that
uses AI.

## Sources

[^1]: ONVIF, [Profile T: advanced video streaming, metadata, events and bidirectional audio](https://www.onvif.org/profiles/profile-t/).
[^2]: ONVIF, [Profile M: metadata and events for analytics applications](https://www.onvif.org/profiles/profile-m/).
[^3]: NATS, [JetStream streams, file retention and duplicate window](https://docs.nats.io/learn/jetstream/your-first-stream).
[^4]: NATS, [Durable pull consumers and explicit acknowledgement](https://docs.nats.io/learn/jetstream/pull-consumers).
[^5]: llama.cpp, [GBNF and JSON Schema constrained generation](https://github.com/ggml-org/llama.cpp/blob/master/grammars/README.md).
[^6]: NIST, [FATE Part 10: passive software-based presentation attack detection](https://www.nist.gov/publications/face-analysis-technology-evaluation-fate-part-10-performance-passive-software-based).
[^7]: NIST, [Adversarial Machine Learning: taxonomy and mitigations](https://www.nist.gov/publications/adversarial-machine-learning-taxonomy-and-terminology-attacks-and-mitigations).
[^8]: Bagdasaryan et al., [Abusing Images and Sounds for Indirect Instruction Injection in Multi-Modal LLMs](https://arxiv.org/abs/2307.10490).
[^9]: Zhang et al., [ByteTrack: Multi-Object Tracking by Associating Every Detection Box](https://www.ecva.net/papers/eccv_2022/papers_ECCV/papers/136820001.pdf).
[^10]: Skantze, [Turn-taking in Conversational Systems and Human-Robot Interaction: A Review](https://doi.org/10.1016/j.csl.2020.101178).
[^11]: Silero Team, [Silero VAD](https://github.com/snakers4/silero-vad).
[^12]: NIST, [Privacy Framework](https://www.nist.gov/privacy-framework/privacy-framework).
[^13]: NIST, [SP 800-207: Zero Trust Architecture](https://csrc.nist.gov/pubs/sp/800/207/final).
[^14]: Security Industry Association, [ANSI/SIA CP-01-2019 false alarm reduction standard](https://www.securityindustry.org/report/ansi-sia-cp-01-2019-standard/).
[^15]: NIST AIRC, [AI RMF Core](https://airc.nist.gov/airmf-resources/airmf/5-sec-core/).
[^16]: Liquid AI, [LFM2.5-VL-450M official model card](https://huggingface.co/LiquidAI/LFM2.5-VL-450M).

## 15. Implementation status (this branch)

The prioritized backlog above was implemented backend-first, in phase order.
No frontend or `/sync` surface for incidents/encounters was added (explicitly
out of scope for this pass).

- **Phase 0 — complete.** The assessment layer is perception-only
  (`vision.describe`, `camera.listen`), every effect goes through one
  `GuardActionAuthorizer` and a persisted outbox, `arm_siren` defaults to
  false, the encounter state column uses the shared `EncounterState` enum with
  a schema-contract test, known `personId` propagates, a greeting only counts
  when the camera acknowledges it and listening/reply depend on it, the gateway
  raw notifier is a heartbeat-gated degraded fallback, hard floors run on a
  fast lane before dialogue/assessment, staging no longer depends on model
  success, and effect budgets are separated from conversational rows.
- **Phase 1 — complete.** Observation contract v2 (`eventId`, per-object
  `trackId`/`observationId`/`dwell`/`zoneKind`), per-frame tracker assignment
  without track reuse, per-track identity cache and crop binding with freshness
  validation, and candidate-only auto-enrollment with an explicit
  `PromotePerson` RPC. Dwell, zone verdict, matcher input and emit cooldown are
  computed per track, so a smaller intruder next to a resident no longer
  inherits the main track's verdict. `PromotePerson` now requires the owner's
  bearer token, verified against the identity database server-side; a declared
  `x-argus-role` is never trusted.
- **Phase 2 — complete.** Durable acknowledged JetStream consumer
  (`NatsBus::subscribeDurable`), serialized observation application, encounter
  revisions and transition history, action outbox with startup reconciliation,
  stale-encounter sweeper, and `Nats-Msg-Id` + duplicate-window publishing.
  The observation saga is now crash-safe: `guard_observation_inbox` carries a
  stage and a JSON checkpoint, every stage persists before the next one and a
  redelivery resumes from the checkpoint instead of repeating side effects;
  incidents, assessments, actions and outbox intents are keyed by `event_id`
  or deterministic `command_id` and written with `INSERT OR IGNORE`; the ACK
  happens after the completed inbox row is durable. Observations the broker
  cannot store are retained in-process by the camera and retried, `publishWithMsgId`
  never degrades to core NATS, and exhausted messages are parked in
  `guard_dead_letter` (plus a JetStream max-deliveries advisory subscription)
  instead of looping forever.
- **Phase 3 — complete except fleet-wide mTLS.** Command ids, encounter ids
  and expiries on the camera action contract; a camera-side idempotency inbox
  whose `sent` rows are the only duplicates and whose `claimed`/`failed` rows
  execute on retry; siren arming persisted as an expiring lease *before* the
  hardware is enabled, rejected without `lease_seconds`, and a sweeper that
  only deletes the lease after a confirmed disarm; `x-argus-service`
  method-scoped authorization. The guard administrative API now runs the full
  device/JSON/JWT/role filter chain and is Owner-only (proxied by the
  gateway); JetStream is mandatory in the deployment (`-js`) and camera
  observations publish only through `js_Publish` + PubAck. The siren sweeper
  never deletes an expired lease while the camera row or its driver is
  unreachable: it keeps the lease, logs an error and retries, so a failed
  disarm can never be mistaken for "off". Arming `arm_siren=true` still
  requires a hardware-side timeout or watchdog (it must alarm-off if the
  process dies), which remains an installation prerequisite. Fleet-wide mTLS
  remains deferred: the PKI currently issues one server certificate for the
  gateway and no per-service client identities, so it needs a dedicated
  PKI/distribution change before it can be enabled everywhere.
- **Phase 4 — complete.** Deterministic risk engine with a closed observable
  tag vocabulary, evidence merging that cannot lower hard floors, scoped
  expected guests (camera/person/host/window, one-time consumption), a real
  `Armed` mode and zone-specific dwell defaults (3 s alert, 12 s monitor, 8 s
  night). Site profiles (home/office/store) are still a config-level
  generalization left for the product pass.
- **Phase 5 — mostly complete.** Grammar-constrained LLM output threaded
  through `ChatRequest` to the llama.cpp sampler (and now fail-closed: an
  unusable grammar yields no text instead of unconstrained sampling), endpointed
  listen capture with `speech_detected`, and encounter states for
  challenging/listening/interpreting. The deterministic dialogue machine owns
  the turns: the encounter stores turn count, last line and last heard text;
  greetings rotate without repeating, listening is skipped when a reply was
  just heard, turns are capped, repair speech is deferred until after the
  assessment, and the spoken line passes a code-side gate (12-word cap, one
  question, no surveillance vocabulary, no private tokens, no digits/URLs, no
  instruction echo) before it can reach the camera. Generated multi-turn
  proposals beyond the bounded challenge/repair loop remain the product pass.
  Silero VAD stays in-process in argus-voice and is not addressable
  cross-service.
- **Phase 6 — mostly complete.** Guard is the single normal notification
  authority; evidence manifests and a daily deletion worker cover guard and
  camera objects; transcripts are stored only at medium+ danger; memory only
  ingests finalized redacted `encounter_closed` summaries. Encryption at rest
  and owner-visible per-capability settings remain deployment/product items.
- **Review round 2 fixes.** The camera owns a durable observation outbox
  (`object_event_outbox` + `camera_event_cooldown`): enqueue and class-cooldown
  advance commit in one SQLite transaction, a worker publishes from the table
  and only marks `sent` after the JetStream PubAck, pending rows resume after a
  restart, and overflow is an explicit `overflow_dropped` state counted in
  `/health` (never a silent pop). The guard parks a message on its **last**
  executable delivery (`delivered >= max_observation_attempts` from NATS
  metadata, matching the consumer's `MaxDeliver`), marks the inbox
  `dead_lettered` and no longer waits for a sixth delivery; a live JetStream
  test proves the max-deliveries path. The incident/guest/checkpoint phase is
  one transaction, effect intents persist their full response (listen
  transcript and `speechDetected` replay without calling the camera again),
  every spoken line (configured or generated) passes the `guard-dialogue`
  gate, the dialogue is goal-driven (`verify_identity → await_reply →
  offer_help | repair_misheard → done`) instead of a call counter, one
  `primaryTrackId` binds rule/identity/zone/crop/encounter end to end in both
  camera and guard, every effect RPC requires a `commandId` (Listen included),
  and `PromotePerson` validates the owner's JWT plus an active device-bound
  session and is reachable through the owner-only
  `POST /guard/person/{id}/promote`. Still open: transactions/fault injection
  for the remaining saga phases, a real owner UI for promotion, and the
  two-person replay corpus.

- **Review round 3 fixes.** Person isolation is structural: the camera emits
  **one event per eligible track** (rule, severity, identity, zone, dwell,
  crop and cooldown bound to that track; companions ride as context that
  never decides), the in-memory cooldown advances only after the SQLite
  outbox transaction returns Recorded, and a failed enqueue keeps the pending
  event for retry. The primary track alone decides known/unknown in both
  camera rules and guard ingestion, so a known companion can no longer shield
  an unknown intruder. `Listen` is a claimed, expiring, replayable command
  (command id + `expires_at`, stored response with transcript and
  `speechDetected`); notifications carry a command id that the receiver
  deduplicates in `notification_command`; the guard parks on the
  second-to-last broker delivery so the last one covers a crash while writing
  `guard_dead_letter`; dialogue is goal-driven (`verify_identity ->
  await_reply -> await_help_reply | repair_misheard -> done`) with a real
  listening window and post-offer response processing; the camera outbox
  health hydrates from SQLite at boot and counts real overflow drops; and the
  AGENTS.md struct/enum deviations were removed (`DurableMessage`/
  `DurableSettlement` handler, `ObservationInput`, `VariantPickInput`,
  `ObjectEventStatus`). Still open: transactions and fault injection for the
  saga phases beyond the incident phase, and the full two-person replay
  corpus against a real camera.

- **Review round 4 fixes.** Track isolation is enforced on the primary only:
  an `exclude` companion can no longer veto the primary event, the primary's
  zone decides the rule, and the legacy frame-wide behaviour is kept only when
  no primary exists. Person cooldowns are boot-scoped
  (`person:<session>:<trackId>`) with an expiry sweep, so a restart cannot
  suppress a new person with a reused track id. Camera action claims are
  atomic with a lease: one executor, `in_flight` for concurrent duplicates,
  `sent` replays the persisted response, `failed`/expired leases re-arm under
  compare-and-swap; `Listen` is claimed, expiring and replayable. The guard
  parks a poison observation at `max(1, maxDeliver - 1)`, preserving the
  broker's final delivery; a live test drives the real `GuardService` and
  proves the dead-letter row and `dead_lettered` inbox state. Notification
  command and batch now commit in one `BEGIN IMMEDIATE` transaction with
  fault-injection tests (failure rolls back the command, retry creates one
  complete batch, duplicates return empty). The dialogue graph is real and
  persisted (`verify_identity -> await_reply -> await_help_reply |
  repair_misheard -> done`) with an integrated fake-camera test. The
  observation outbox distinguishes inserted/duplicate/overflow and hydrates
  health counters from SQLite before the publisher starts. `eventId` is
  assigned once to the retained pending item and reused across failed
  retries. Remaining honest gaps: crash injection at every individual saga
  boundary beyond the tested encounter/assessment/evidence/intent paths, and
  a hardware-in-the-loop replay.

- **Phase 7 — partial.** Adversarial parser/risk fixtures, multi-person and
  endpoint-detector tests, schema/transition contract tests, dialogue-line gate
  tests and an idempotent observation-saga suite ship; the private replay
  corpus, shadow-mode rollout and hardware-in-the-loop lease tests require the
  installed camera and are the next operational step.
