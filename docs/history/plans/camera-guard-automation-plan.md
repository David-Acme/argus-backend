# Camera guard automation plan

Autonomous camera security for Argus: the user connects a camera and Argus
watches, recognizes, assesses and intervenes on its own. This document is the
source of truth for the phased implementation; each phase ships behind flags
so the owner can test stage by stage with a real camera.

## 1. Current state (as-is)

```
go2rtc (/api/frame.jpeg?src=cam<id>)                    [argus-camera]
  → YOLO26n ncnn (Vulkan, 2 fps, conf 0.45)
  → EventIntelligence (9 fixed rules, TOML zones, night, cooldown 30 s, 5 s window)
  → NATS argus.camera.v1.object_detected (JetStream ARGUS_CAMERA, best effort)
       → gateway camera-notifier: budget 6/camera/rolling hour, silent hours, digest
            → notification rows (owner + guard) → /sync (no OS push; push off by default)
```

Known gaps this plan closes:

- `IKnownPersonMatcher` is `NoKnownPersonMatcher`: every person is unknown and
  `knownPersonId` never appears (`services/argus-camera/src/operator/known-person-matcher.hxx`).
- `zone` rows edited in the app never reach the detector: the operator reads
  only `[operator].zones` (TOML JSON).
- Camera discovery is a boot snapshot; a camera connected later is never
  monitored (`camera-operator-service.cc`).
- No unknown-person store, no danger level, no intervention, no memory of
  sightings. `event` / `person_event` are retired argus.db leftovers.

## 2. Target architecture

```
[argus-camera]  perception + identity enrichment
  motion gate → YOLO → temporal smoothing / static-box filter / DB zones
    → person crop → identity.IdentifyPerson (throttled)
        → match: known person id | no match: optional identity.EnrollPerson
    → object_detected enriched: environment, per-object identity, track/dwell
       ├→ [argus-guard]  new service, owns guard.db
       │     hard floors + context + soft assessment → danger level → actions
       │     actions: notify | announce (TTS) | alarm tone | arm siren (opt-in) | PTZ
       └→ [argus-llm/memory]  camera episodes (system observations only)
```

Only one new service (`argus-guard`); everything else reuses existing
microservices, SDKs and packages.

## 3. Performance: strict cascade, heavy AI last

False positives are the product enemy: more than roughly two false alarms per
camera per day and the alert channel is ignored. Every stage exits early.

| # | Stage | Cost | Runs when | Early exit |
|---|-------|------|-----------|------------|
| 0 | Motion gate (downscaled frame diff or Tapo motion) | ~1 ms | always | no motion → stop |
| 1 | YOLO26n (existing) | medium | `max_fps_inference` and motion | no person/vehicle class → stop |
| 2 | Temporal smoothing + ROI zones + static-box filter + IoU tracking (no AI) | ~1 ms | detections | screen/painting/reflection or ignored class → stop |
| 3 | Face identity (existing engine) | medium | person present, throttled `identify_interval_ms`, cached | known with tags → stop (no VLM) |
| 4 | Hard floors + cheap context (zone, hour, site mode/occupancy, expected guests, repeats) | ~0 | unknown | floor ≥ alarm → act without VLM |
| 5 | VLM describe (384 px, strict prompt) | high | unknown ambiguous, once per encounter | clear threat → decide |
| 6 | LLM (1.2B) with memory/history | very high | `mode=llm` and VLM ambiguous | JSON `{threat, tags, veto}` |
| 7 | Actions (notify, TTS, tone, siren, PTZ) | low | by level and caps | — |

Cross-cutting rules: heavy work off the event loop (`BlockingTask`), thread
budgets from `ThreadBudget`, VLM/LLM with timeout and best-effort skip when
busy (hard floors still decide), assessment cached per person/tag and per
encounter, per-stage latency logging, voice never blocked.

## 4. Site profiles

`guard.profile` generalizes `default_mode` so Argus also fits offices and
stores.

| Axis | home | office | store | custom |
|------|------|--------|-------|--------|
| Schedule | 24/7 + night | business hours | open/close | configurable |
| Unknown during open hours | by environment | normal (0) | normal (0) | flag |
| Unknown after hours | escalate | escalate | escalate + concealment/loitering | flag |
| Sensitive zones | alert/monitor/exclude | restricted rooms | back office/register | free |
| Announce/alarm | from level 3 | level 3 | level 3 + deterrence | configurable |
| Mode | home/away/night | open/closed | open/closed | manual |

## 5. Danger model: hard floors, soft assessment, audit

- Hard floors — never vetoed by AI: interior unknown alone (more so in `away`
  mode), unknown in `alert` zone, aggression/weapon cues from the VLM,
  recurrence, unknown following a resident, multiple unknowns.
- Soft assessment — may relax only soft scenarios: exterior, no alert zone, no
  recurrence, daylight; gated by `veto_scope = "soft_only"`.
- Anti-deception: every assessment persists in `guard_assessment`; if a hard
  floor fires while the model said "not aggressive", it still escalates and the
  person is tagged (deceptive calm). The VLM is advisory, never the sole judge.
- Scene-text prompt injection (a sign or shirt telling the model to stand down)
  is mitigated because the VLM can never silence a hard floor, assessments are
  audited, and the crop may be text-attenuated before inference
  (`ocr_text_defense`).
- Facial spoofing: a photo or screen of a resident can pass the match. A known
  face is a signal, not proof: known-while-away escalates and the VLM looks for
  held photos/phones.
- Confidence: when the assessment is ambiguous the system notifies (level 2/3)
  instead of silencing.

## 6. Occupancy, expected guests and memory

- `guard_mode`: `home | away | night | armed` (+ office/store modes per
  profile), manual first through the API, app later.
- Future automatic presence (`presence_source = "devices"`) from devices
  connected to the tunnel/sync; no presence signal exists in the system today.
- Expected guests: `guard_expected_guest` windows (description, optional
  person). A resident telling the assistant "someone is coming" lives in
  `argus-memory`; the LLM assessment (`use_memory_context`) recalls it as
  context, never as a hard-floor veto.

### 6b. Stranger memory isolation (binding)

Only utterances from authenticated users can become LLM memory. A person the
system sees through a camera is foreign to the system: their words, intents and
recall requests must never create facts, preferences, reminders or episodes in
`argus-memory`. Camera events enter memory only as system observations
(`observeSystemEvent`, actor = camera, episode only — never rule-parsed facts).
No camera audio path may feed the chat/memory capture pipeline without an
authenticated user scope.

## 7. Flags

```toml
# argus-camera
[objects] enabled = true
[operator] zones_from_db = true, track_persons = true, static_box_ms = 4000,
           motion_gate = false, motion_min_ratio = 0.002
[identity] identify = false, auto_enroll = false, capture_clear_faces = true,
           track_all_persons = true, identify_interval_ms = 2000,
           min_face_box_px = 48, enroll_cooldown_ms = 600000
[health] enabled = true, interval_ms = 60000
[actions] enabled = false

# argus-guard (new)
[guard] enabled = false, profile = "home"
[guard.presence] source = "manual", default_mode = "home"
[guard.expected_guests] enabled = true, max_window_hours = 24
[guard.assess] enabled = true, mode = "vlm", veto_scope = "soft_only",
               use_memory_context = true, ocr_text_defense = true
[guard.actions] notify_level = 2, announce_level = 3, alarm_level = 4,
                arm_siren = false, notify_critical_bypasses_silent = true,
                max_actions_per_hour = 4, snapshot_retention_days = 30

# argus-llm
[memory] observe_camera_events = false
```

## 8. Case catalog

| Context | Level | Action |
|---------|-------|--------|
| Interior + away + unknown alone | 4 | announce + alarm + notify + tags |
| Interior + home + unknown alone | 4 | announce + alarm |
| Interior unknown with owner/resident | 2 | assess; 4 on aggression |
| Interior unknown with guest | 3 | announce; 4 on recurrence/aggression |
| Exterior unknown alone, day, no signals | 1 | assess; neighbor/parcel → incident + tags |
| Exterior loitering > 20 s, night or alert zone | 3-4 | announce/alarm |
| Exterior unknown with resident | 1 | tags; 4 on aggression |
| Expected guest window | 0-1 | incident linked to host |
| Unclear face (no embedding) | — | person row, snapshot + VLM tags, no future match |
| Known person alone | 0 | bump `last_seen` |

Additional handled cases: TV/screens showing people, paintings, glass
reflections, light changes, IR insects and cobwebs, occlusion/blur/moved
camera/dead IO, a resident's photo held to the lens, crouching/back turned/
carrying a box, multiple unknowns, unknown following a resident, learned
routines (delivery tagged by the LLM), alert fatigue budgets, false-positive
feedback, snapshot retention.

## 9. Phases

1. **Camera**: camera reload over NATS, DB zones, geometric tracking (IoU,
   dwell), motion gate, static-box filter, health monitor.
2. **Identity**: `Identify/Enroll/Touch/Tag` RPCs, `person_tag`, snapshots.
3. **Guard**: service skeleton, `guard.db`, incidents, notification action.
4. **Guard**: VLM/LLM assessment, tags, veto audit, anti-deception.
5. **Guard**: profiles, occupancy, expected guests, interior/exterior and
   companion matrix.
6. **Actions**: camera internal RPC, announce, alarm tone, opt-in siren with
   auto-disarm, caps; AGENTS/docs amendments (audible intervention only from
   argus-guard through fleet-gated RPCs, never in tests).
7. **Memory**: camera episodes, routines, false-positive feedback.

Each phase carries its own flags and acceptance test with the real camera.

## 10. Reuse map

| Piece | Reused from |
|-------|-------------|
| Frames, motion gate, YOLO, zones, overlay | `argus-camera` operator + StreamHub |
| Recognition, embeddings, persons | `argus-identity` (FaceService/FaceDB, new RPCs) |
| Private snapshots | `PrivatePortraitService` / `stored_file` |
| Events and flags | `NatsBus`, `ConfigService` |
| Alerts | `NotificationClient`, `/sync`, gateway budget/digest |
| Speaker and TTS | `TapoTalkClient`, `argus-tts` |
| Assessment and context | `argus-vlm` describe, `argus-llm` chat + memory |
| Recalls | `MemoryService::observeSystemEvent`, `CatalogReplica` |
| Audit and incidents | `argus-audit` and sync (UI later) |
| Compute budget | `ThreadBudget` |

## 11. Explicit non-goals (initial)

Re-identification without a clear face (body/clothing), full facial
anti-spoofing, reliable weapon detection with a 450M VLM, physical access
control/locks, and multi-camera correlation. Each stays a future phase behind
its own flag.

## Status

- Phase 1 (camera): DB zones, live camera reload, motion gate, static-box
  filter — shipped.
- Phase 2 (identity): Identify/Enroll/Touch/Tag RPCs, `person_tag`,
  `person_snapshot`, matcher with auto-enroll, per-object identity — shipped.
- Phase 3 (guard): service, `guard.db`, danger matrix, incident/action
  persistence, notification action, mode API — shipped.
- Remaining: health monitor, expected-guest windows, VLM/LLM assessment with
  veto audit, camera action RPC (announce/alarm/siren), memory episodes,
  audible-rule amendments.

Updated status: phases 1-7 are implemented. The camera health monitor, the
snapshot store and the fleet-gated camera action surface; the guard's expected
guests, VLM/LLM assessment with audited soft veto and the audible action
ladder; the audited AGENTS amendments; and the argus-llm camera-episode
observer all ship in this branch. Remaining future work is the honest
non-goals list (section 11) plus the mobile UI for visitors/incidents.

## Guardian flow v2: dwell-gated, staged, conversational

Principles: no expensive AI without a value signal; one evaluation per
encounter, never per frame; identity before escalation; conversation before
alarm whenever the case is soft; learned tags feed the next encounter.

### Stage 0 — cheap perception (argus-camera)

- Motion gate + YOLO at `max_fps_inference`, exclude zones drop screens,
  reflections and the TV region.
- A geometric IoU tracker keeps a per-person track: `trackId`, `dwellMs`,
  movement (static/approaching/leaving). The static-box filter removes frozen
  boxes.
- Zone semantics: `alert` fires immediately; `monitor` requires dwell;
  `exclude` drops. Thresholds: `dwell.alert_ms` (default 3000),
  `dwell.monitor_ms` (default 12000), `dwell.night_ms` (default 8000).
- The camera emits an event when a threshold is crossed, then a heartbeat
  every `recheck_ms` (default 30000) while the same track stays present.

### Stage 1 — scene understanding (VLM, once per encounter)

- The person crop goes to the VLM with the strict security prompt.
- The LLM turns the description into structured JSON:
  `{gravity, threat_type, description, needs_intervention, confidence, reason}`.
- If `needs_intervention=false`: record an "observed" incident and schedule a
  re-check; after `observation_count >= 3` cumulative checks the case escalates
  as loitering even without an alarm-worthy signal.

### Stage 2 — identity resolution (whenever possible)

- A clear face is matched (threshold 0.80): resident/owner/guard -> close the
  encounter (optional greeting, no alarm); known non-resident (neighbour,
  tagged) -> soft conversational flow; unknown -> Stage 3.
- Without a clear face the person stays unknown but gravity comes from zone,
  night, dwell, behaviour and prior tags.

### Stage 3 — conversational intervention (LLM agent)

- Tool set: `vision.describe`, `camera.announce` (speak),
  `camera.listen` (STT), `camera.alarm`, `camera.siren`, plus the final
  decision with tags.
- Bounded conversation: `max_exchanges` (default 3) or
  `conversation_timeout_s` (default 60). A coherent answer (delivery,
  neighbour, expected guest) tags the person and de-escalates to a soft
  notification; silence, evasion, aggression or persistence in an alert zone
  at night escalates to alarm tone and, when the case merits it, the siren.
- Hard floors are never vetoed: away + unknown, alert zone, VLM aggression
  cues and recurrence go straight up.

### Stage 4 — close and learn

- Incident, actions and assessment are persisted; tags are written to the
  person so the next encounter starts with context (delivery, neighbour,
  calm). Notification budget and digest already apply.

### False-positive controls

1. Dwell gate (10-15 s) before any VLM/LLM work; passers-by trigger nothing.
2. Motion gate, exclude zones and the static-box filter remove screens,
   reflections and light changes.
3. Identity runs before escalation: residents and known people are filtered.
4. One state machine per encounter with cooldown and scheduled re-checks.
5. The soft conversation lets delivery/neighbour cases de-escalate.
6. Hard floors only fire on strong signals (away, alert zone, night
   persistence, aggression, recurrence).
7. "Observed" outcomes do not notify; budgets and digest cap the rest.
8. Tags turn returning visitors into context instead of new alarms.

### Implementation gaps to close next

- Camera: dwell tracker (`trackId`, `dwellMs`, zone kind), threshold events
  and recheck heartbeat in the event payload.
- Guard: encounter state machine (`observing -> assessing -> conversing ->
  escalating -> closed`) with re-check scheduling and observation counters.
- Agent: prompt carrying the staged flow and the conversation budget; tools
  already exist.
- Config: dwell/exchange/timeout defaults live in code; config only tunes
  them.

## Adaptive cadence, best shot and multi-camera (v2.1)

- **Adaptive inference**: the operator runs at `objects.max_fps_inference`
  (idle), `objects.active_fps` while motion is being processed and
  `objects.burst_fps` for `objects.burst_ms` after a person appears, so short
  clear-face windows are not missed without paying 30 fps all day.
- **Best-shot capture**: the identity matcher scores every crop (box area x
  Laplacian sharpness), keeps the best result in a `identity.best_shot_ms`
  window and only re-identifies when a clearly better frame arrives
  (`identity.improve_margin`); the clearest frame of the encounter is what
  reaches the face engine.
- **Appearance signature**: the camera adds a compact Lab color histogram
  (`objects[].signature`) to person events for cross-camera correlation.
- **Encounter correlation (guard)**: each event resolves to a `guard_encounter`
  by face `personId` first, otherwise by signature similarity
  (`guard.signature_min_similarity`) inside
  `guard.cross_camera_window_s`; a person seen by several cameras is one
  encounter, one assessment and one action budget.
- **Staged observation**: a calm assessment below the hard floors keeps the
  encounter in `observing` (incident without notification) until
  `guard.loiter_checks` re-checks; then it escalates as loitering. Actions are
  gated per encounter, and a known resident closes their encounter.

### v2.2 — natural fast greeting

- **Fast path before any model**: a first-seen unknown in `home` mode gets a
  short natural line from the configured variants (`guard.greet_texts`,
  rotated per encounter) within ~1 ms of the event; known residents get
  `guard.greet_known_text` when `guard.greet_known = true`. Recording,
  assessment and danger never enter the spoken line.
- **The greeting asks, the guard listens**: after the announce the camera
  captures `guard.greet_listen_seconds` of microphone audio and the
  transcript joins the assessment context, so the model can answer what the
  visitor said instead of narrating the automated process.
- **Vision first, one model turn**: the person crop and its VLM caption are
  gathered before the LLM call, which now closes in a single JSON turn
  (`guard.assess.max_tool_rounds` caps the rest). The guard passes
  `tools:false` on the LLM wire so the memory-tool preamble (~400 tokens)
  never enters the prompt.

### Measured latency (Ryzen 7 5825U, prod containers, fake camera transport)

| Metric | Before | After |
| --- | --- | --- |
| Event -> announce RPC | (after full assessment) | 1 ms |
| Greeting audio (TTS) | 3.4 s every time | cached, ~1 ms (first 3.3 s) |
| Reply -> transcript (STT) | 0.5 s | 0.2 s |
| LLM turn | 11-13 s x 1-2 | 5.4 s x 1 (`tools:false`) |
| Event -> assessment | 23.5 s | 13.5 s |
| LLM dev (Debug) vs prod (Release) | 75.5 s | 5.7 s (13x) |
| VLM dev vs prod | 12.1 s | 3.9 s (3.1x) |
| TTS/STT dev vs prod | ~1x | prebuilt third-party libs |

The tool preamble and prefix reuse findings live in the LLM wire docs; the
caches are noted in the guard, TTS and camera CONTEXT files.
