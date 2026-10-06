# Selectable modules and the first-run welcome

Status: approved in conversation with the owner on 2026-10-06; this document fixes the contracts so
the backend and the app can be built in parallel.

## Goal

A new Owner sets Argus up from the phone in a short, warm, visually attractive welcome and chooses
which modules the installation runs. Only the chosen modules are installed, processed and shown; the
installation runs on the server, survives the app being closed and the server restarting, and the
app shows its real progress at any time. An invited user meets a shorter welcome and never chooses
modules. The design scales to future first-party modules (agronomy is listed now as "coming soon").

Out of scope: third-party plugins, push notifications, the tunnel, starting or stopping containers.

## Vocabulary

- **Component**: an installable technical piece (`voice-stt`, `voice-tts`, `llm`, `vision`,
  `detector`). It names the service that owns it, the files it needs (path relative to that service's
  models dir, size in bytes, SHA-256, source URL pinned to a revision) and the RAM it needs loaded.
  `source` is `download` (the owning service fetches it itself) or `provisioned` (only the host
  provisioning can produce it, e.g. the YOLO NCNN export; the app shows the host command).
- **Module**: what the user sees. It names its components, the modules it requires, its hardware
  requirements (minimum and recommended RAM, free disk, optional CPU/GPU features), its gated route
  prefixes and its state in the catalog: `core`, `available` or `coming_soon`.
- **Core**: the always-installed module (account, people, sync, notifications, settings, privacy
  and the voice assistant: `voice-stt`, `voice-tts`, `llm`). It cannot be disabled.

Modules at launch: `core` (core), `surveillance` (available; requires `core`; components `detector`,
`vision`; gates `/camera`, `/zone`, `/media`, `/guard`, `/visitor`, `/visitor-settings`,
`/visitor-crop`), `productivity` (available; requires `core`; no components; gates `/project`,
`/project-task`, `/project-member`, `/calendar-event`, `/calendar-event-share`), `agronomy`
(coming_soon; nothing to install).

## The catalog is data

`services/settings/modules.json`, beside `profiles.json`, validated at boot like the profiles
(unique ids `[a-z0-9-]`, every required module and component known, no dependency cycle, every
`download` file pinned with size and SHA-256). Adding a module is a catalog entry plus its code; the
welcome and the modules screen render whatever the catalog says.

## Ownership

- `argus-settings` is the **module manager**: catalog, dependency resolution, hardware check, the
  persistent job queue, the enabled set, the REST surface and the progress events. It owns a small
  database of its own (`settings.db`: `module_state`, `module_job`, `module_audit`).
- Each service **installs its own components** through the settings wire (rule 27: nobody writes
  another owner's models dir). The wire gains `ComponentStates` (installed / missing / installing,
  bytes present) and `InstallComponent` / `CancelComponent` (start or stop fetching one component;
  progress is read back with `ComponentStates`). A `download` component is fetched by the new
  tier-1 `lib/fetch`; a `provisioned` one answers `hostOnly` with its command.
- `lib/fetch` (libcurl from Conan, tier 1): one call downloads a pinned file to `<target>.part`
  with a `<target>.part.json` sidecar (`{url, size, sha256, etag}`), resumes with `Range` (206
  appends, 200 truncates and restarts), reports bytes through a callback, honours a cancellation
  token, verifies SHA-256 and renames atomically. Redirects followed, TLS verified, bounded retries.
- **Gating**: `role_access` maps route prefixes to modules; `RoleFilter` refuses a route of a
  disabled module with 403 `MODULE_DISABLED` (no new filter, the chain of rule 5 is unchanged). The
  enabled set reaches every service through the durable NATS subject `argus.settings.v1.module`
  plus a boot read over the settings wire, cached per process. A disabled module also stops its
  background processing (the camera operator stops analysing, guard stops evaluating). Data is kept.

## Jobs

One job per install, persisted, one running at a time, ordered by dependencies. States:
`queued → checking → downloading → verifying → activating → health_check → done`, with `paused`,
`failed` (with a reason code) and `cancelled`. On boot unfinished jobs resume. Progress is the sum
of bytes present over the sum of bytes needed across the module's components; it never decreases.
A failed step leaves the module as it was (never half enabled). Enabling ends with each component's
service confirming it loaded the model (`ComponentStates.ready`).

Hardware check before queueing, from the existing hardware probe: free disk on each target models
dir ≥ remaining bytes + 10 %, RAM against minimum/recommended counting what is already installed,
CPU/GPU features when declared. Verdict `ok`, `slow` (allowed, explained) or `insufficient`
(refused 409 `MODULE_HARDWARE_INSUFFICIENT`, explained).

## REST (argus-settings, TLS, filters `DeviceFilter → JwtFilter → RoleFilter`)

| Route | Who | Answer |
|---|---|---|
| `GET /modules` | every role | Owner: the full catalog with state, hardware verdict, sizes and the active job. Other roles: `[{id, name, enabled}]` only. |
| `POST /modules/{id}/install` | Owner | 202 with the job; 409 `MODULE_HARDWARE_INSUFFICIENT`, 409 `MODULE_COMING_SOON`, 409 `MODULE_JOB_RUNNING` |
| `POST /modules/{id}/pause`, `/resume`, `/cancel` | Owner | the job |
| `POST /modules/{id}/disable` | Owner | the module; 409 for `core` or a module another enabled one requires |
| `POST /modules/{id}/release` | Owner | frees a disabled module's downloaded files (explicit, confirmed in the app) |

Module JSON: `{id, name, summary, kind: core|available|coming_soon, enabled, requires[], sizeBytes,
installedBytes, hardware: {verdict: ok|slow|insufficient, reasons[], minRamMb, recommendedRamMb,
freeDiskMb}, job: null | {id, state, progress (0-1), bytesDone, bytesTotal, bytesPerSecond,
etaSeconds, reason}, gettingStarted[]}`. Names and summaries come back in the caller's language
(es first, en).

## Live progress over `/sync`

New additive operation `ModuleUpdate = 12` (`module_update`), emitted to the Owner's room with the
module JSON above (throttled to one frame per second or per 1 % per job) and, when the enabled set
changes, to every connected socket with `{modules: [{id, enabled}]}`. The app paints its cached
modules first, calls `GET /modules` on open and after reconnect, then follows the frames; while a
job is active and the socket is down it polls `GET /modules` every 3 s. On `done` or `failed` the
Owner also gets an in-app notification.

## The app

- **Welcome**: a visually rich first screen (the brand, a warm one-line promise, "everything stays
  in your home", one button) consistent with the existing design system and dark/light themes.
- **Data-driven steppers**: a flow is a list of steps `{id, component, when, skippable}`.
  Owner flow: pair → privacy → face → modules → meet Argus (5 counted steps). Invited flow:
  invitation → privacy → face → meet Argus (4). The modules step shows one card per catalog module
  with the hardware verdict, size, what it adds and its dependencies (resolved automatically);
  confirming queues the jobs and the flow continues while the server installs.
- **Settings › Modules** (Owner): the same cards with state, live progress (percent, MB of MB,
  speed, ETA), pause/resume/cancel, disable with confirmation, free space, retry on failure.
- **Navigation** shows only enabled modules; a disabled module's screens are unreachable and its
  synced data stays in the local database.
- **Getting started**: each installed module contributes its first steps from the catalog
  (`gettingStarted`), shown on the home screen until done or dismissed.

## Internal-test hardening carried with this work

- A role change ends an open WebRTC view (the camera closes its consumer like a revocation).
- Every flow above has unit tests; the native sandbox replays the new routes in the goldens.
- Device validation (face liveness threshold on real selfies, credential flow, real camera, voice
  call) is a checklist for the owner's internal test, not something a build can prove.
