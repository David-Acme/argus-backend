# Live user context, growing roles, module-aware tools (MCP) and voice quality

Status: agreed with the owner on 2026-10-06. Follows `modules-and-welcome-plan.md`. Object search
and talking to Argus through a camera are later plans; their decisions are recorded at the end.

## 1. Live user context over the socket

- The first frame on `/sync` (`InitialInfo`) carries the user's whole context:
  `context: {userId, role, roleActive, capabilities[], modules[{id, name, summary, intro, enabled,
  lifecycle, dataPurgedAt}], ownerCatalog?}` (`ownerCatalog` is the Owner's full module JSON with
  hardware and jobs). New additive operation `ContextUpdate = 13` (`context_update`) sends the same
  `context` whenever the role, a module or a capability changes; `ModuleUpdate = 12` keeps carrying
  install progress to Owners. The app reads module state only from the socket (no `GET /modules`
  polling); actions stay HTTP. `GET /modules` remains for tools and the goldens.
- Offline devices paint the cached context and reconcile on `InitialInfo` without flicker. Before
  any context exists the app shows only core.

## 2. Roles

- `UserRole` stays an enum in code and grows with modules; the database stores the role as plain
  text **without any CHECK** (identity `user.role`, `user_invitation.role`; table rebuild with
  foreign keys off outside the transaction, `foreign_key_check` after, backup first; no cascade may
  fire). AGENTS.md rule 1 is amended for `role`.
- An unknown role (not in the enum) gets **no permissions**; nothing defaults to Guest any more.
- The module catalog declares which roles a module brings: `guard` belongs to `surveillance`. A role
  whose module is not active is **inactive**: the user keeps it, the server grants core only, and the
  app shows a calm animated screen that explains it and offers nothing else.
- Uninstalling a module whose roles are held asks the Owner to reassign those people first (the
  impact preview lists them); pending invitations for that role are revoked with a reason the
  inviter and the invitee both see ("disabled because the Surveillance module was turned off").
  Invitations are single use and never come back.

## 3. Capabilities: role ∩ active modules, computed once

`role_access` keeps its enum tables and gains `capabilitiesFor(role, activeModules)`: the one
answer for what a user may use now (`camera.view`, `camera.talk`, `guard.read`, `guard.mode.set`,
`safety.panic`, `safety.duress`, `visitors.read`, `agenda.read`, `projects.write`,
`reminders.read`, `directory.read`, `presence.read`, `response.duty`, …). The server stays the
authority (routes, sync pulls, tools); the app only hides. The app gets the list in its context
and has one `useCapabilities()` every screen reads.

- Core gains the **panic button** (`/guard/panic` is never gated; guard keeps running to serve it);
  duress, PINs and disarming stay in surveillance.
- **Reminders** are core: a Reminders section on home always; the agenda also shows them when
  productivity is active.
- A disabled module stops everything it was doing: live views and talk, agenda calls, guard digests
  and duty, pending alerts; its notification kinds and history are hidden (kept), its settings
  groups, privacy signals (stored values kept) and call preferences disappear, its tables stop
  syncing to devices (kept on the server).
- **Impact preview** before disable/uninstall, in the app and by voice: what stops, who loses a
  role, which invitations are revoked, what data stays.
- Welcome: privacy comes after the modules step and asks only about installed modules; every module
  card has a short intro (what it is, 2-3 examples, optional "see more").

## 4. Activity history (audit)

`user_action_log` (sync) gains a `module` column; settings' module actions are journaled into it.
Owner-only HTTP `GET /sync/activity` with filters (module, user, action, table, from, to) and keyset
paging, because the volume is large. The app gets an Activity screen with those filters.

## 5. Tools over MCP, one assistant for the whole system

- One LLM serves the whole system; modules are **tool providers**. A first-party `lib/mcp`
  (JSON-RPC 2.0, MCP `tools/list` and `tools/call` over the internal HTTP/gRPC transport with
  per-caller credentials) lets each service expose its tools; `argus-llm` is the MCP client that
  aggregates them. Tools are filtered per turn by the caller's capabilities, so a disabled module's
  tools are not offered.
- When a user asks for something whose module is off, Argus says so naturally; for the Owner it
  offers to enable it ("Productivity organizes your agenda, projects and tasks; shall I enable
  it?"), installs on a clear spoken yes, keeps the original request as a **pending intent** on the
  server and completes it when the module is ready (e.g. saves the meeting). If the install fails,
  the request is kept as a core reminder. A non-Owner gets "shall I ask the household owner?" and
  the Owner receives a notification with an Enable action.
- Destructive actions by voice always need a clear spoken confirmation and the impact summary;
  purging data is never done by voice: Argus opens the purge screen instead.
- The fast tier stays: fastText routes the common commands quickly; the LLM handles the rest and
  writes the arguments. Both are measured (section 6).

## 6. Voice quality, measured

- An evaluation harness with gates: tool selection precision/recall per tool, false-action rate
  (actions nobody asked for), argument accuracy, spoken-confirmation compliance for destructive
  tools, for both the fastText tier and the LLM, Spanish first (Peruvian Spanish included) and
  English. A model or prompt change that worsens a gated number fails the build.
- Data: the existing labelled sets plus real usage, adapted public Spanish datasets (MASSIVE es,
  others with permissive licenses), new labelled utterances for the module tools and the
  "module not active" cases. fastText is retrained in the sibling `intent-training/` project and
  republished with its card; the deploy mounts `models/intent` so the fast tier runs in production.

## Later plans (decisions already taken)

- **Object search** ("I don't know where I left my keys"): YOLO for common objects, the VLM for
  the rest and to describe the place, across cameras, with a recent-frames window; cancellable.
- **Talking to Argus through a camera**: cascade VAD → keyword spotting of "Argus" → STT; the
  speaker is identified by face on that camera; when not visible the voiceprint is only a hint:
  Argus always asks ("are you Ana?") and asks where she is to move the camera to verify her face.
  Every action needs confirmation and an authenticated face; sensitive actions (disarm, purge) are
  refused at a camera and sent to the app.
- **Updating backend modules** (versions, migrations): a future plan.
