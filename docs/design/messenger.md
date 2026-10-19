# The messenger programme: Argus carries messages between household members

Status: design note, 2026-10-14. Step 1 of the messenger programme (F8 of `docs/history/plans/realtime-and-backlog-plan.md`,
detailed in `docs/history/plans/parallel-conversations-plan.md` section 6). Written from the code at master `cc798138`
and the two plans. Nothing here is built by this note. Phase 2 starts only after the reviewer rules on section 8.
Every decision is stated as a decision; open questions come last, each with a recommended default.

## 0. Scope

**In.** Argus as the household's messenger between registered members: "Argus, llama a David y dile que venga a comer"
and "dile que ya voy" as the answer. The message travels by the best channel the recipient has at that moment (an open
conversation, then a notification). Phase 2 builds the channels that exist without the watchman or F9.

**Out.**
- The watchman: seeing a recipient through a camera, speaking through a camera, a visitor at a door
  (`docs/history/plans/camera-watchman-plan.md`). The camera channel is named here and left empty in phase 2.
- Calls from outside: a relayed message never rings a device, and the call engine is not used for it (section 1.3).
  Calls that Argus starts for its own reasons (intruder, arrival, agenda, scheduled reminder) keep their engine unchanged.
- The recorder (`docs/history/plans/argus-recorder-design.md`, not this programme).
- Conversation-per-person and parallel LLM sequences (F9). Phase 2 names the interface it needs; it does not build it.

## 1. The decisions

1.1 **The router picks the tool; the LLM never does.** The relay is a tool, `message.relay`. It is chosen by the rules
tier (section 2.2) or by the router (Laya, section 2.1) and filled by slots. The LLM only speaks about the result,
as with every other tool in `services/llm`.

1.2 **Slots.** The recipient is resolved with `text_norm::matchName` over the household roster read through
`argus::clients::identity` (rule 27: llm never opens identity's database). Exactly one match fills the slot; zero or
several matches make the turn ask one question through the existing slot machinery (`slots::Filled`, `Dispute`).
The message is the sender's own words, taken from the utterance after the trigger phrase. Nothing else from the
sender's context travels: not the memory block, not the summary, not a fact.

1.3 **Delivery, best channel first, never ringing.**
- (a) The recipient's open conversation, if F9's `Conversation` exists and the recipient has an active leg. The
  message is spoken there: "Ana te manda decir que vengas a comer." Phase 2 reaches this through the port in section 6,
  and until F9 lands the port answers "no open conversation".
- (b) The camera that sees the recipient. Not in phase 2 (the watchman).
- (c) A notification row, type `household_message`, through `CreateNotifications`. It reaches the recipient's app
  through the existing `Add` change feed and, if push is enabled, through the existing push path. A relayed message
  is never a `call` push and never a `CallIncoming` frame.
- The call engine (`services/notification/src/feature/call/`) is not called by the relay. A test pins this (section 5).

1.4 **The answer travels back.** A relay writes a `relay_thread` row (section 3.3) with the sender, the recipient,
the message id and an expiry (`messenger.reply_window_s`, default 300 s). A reply ("dile que ya voy") goes back only when
EXACTLY ONE thread is open for the replier within the window. With two or more, Argus asks "¿a quién?" through
`slots::choose` among the senders, and the answer picks that sender's thread. With none, the reply is a new message with
no recipient, and Argus asks who. There is no guess of the newest thread. An answer after the window is a new message.

1.5 **The capability.** `messages.relay` is a core capability in `packages/lib/auth/src/auth/capability.hxx`, granted
to every registered household role (Owner, Resident, Guard, Guest). `Unknown` holds nothing (section 4.1). The tool's
`ToolSpec.capability` is `messages.relay`, so `tool_access::runnable` decides whether the tool is offered at all.

1.6 **Strangers get no information.** A visitor (a turn with no account, at a camera) is offered no relay tool and
is told nothing about the roster. A visitor who asks for someone ("¿está David?") gets no information: not whether that
person is there, not where. Argus asks what they need and gives the same neutral line whether or not the name matched.
If the name matches exactly one household member, that member is notified with a `household_message` from "un visitante
en <cámara>" carrying the visitor's answer (or the member's open conversation receives it, section 1.3). If the name
matches no one, or several members, nobody is notified. The visitor never learns whether a notice was delivered. This is
the plan's section 6 "Strangers" rule, adopted by the ruling of 2026-10-14.

1.7 **Presence chooses the channel and is never revealed.** The sender hears only one of three outcomes:
"se lo dije" (delivered in the open conversation), "le dejé el mensaje" (notification), or "no contestó, le dejé el
mensaje" when the recipient has not answered by the reply window. Presence (`presence.read`, Owner-only in
`/guard/presence`) is not read by the relay at all in phase 2; the channel choice uses only the open-conversation port,
which says whether a leg is active and nothing about where it is.

1.8 **Relay carries no memory.** The relayed text is the sender's utterance slot. It is never the memory block, the
per-turn context, or a fact of the sender's `scope = 'user'` memory. A test pins the payload (section 5, MS6).

## 2. What is reused, by module path

### 2.1 Router and slots

- `services/llm/src/feature/llm/services/turn/decider.hxx`: the `Decider` interface, `Candidate`, `DecideInput`
  (`offered` tools, `modules`).
- `services/llm/src/feature/llm/services/turn/deciders.cc`: `RuleDecider` (module-command and app-command families,
  `isOffered`), and `RouterDecider`, which maps the fastText-era `intent::ToolIntent` enum to tools through
  `memoryCall`. The router path is a `switch`; a new intent needs a new enum value.
- `services/llm/src/feature/llm/services/turn/laya-decider.cc`: `kLabelTools`, a closed array of 21 label-to-tool pairs.
  The Laya bundle is trained outside this repo (`intent-training/`). The label set is the only coupling
  (`services/llm/CONTEXT.md`, "The label set is the only coupling"), so a `message_relay` label needs a new bundle.
- `services/llm/src/feature/llm/services/turn/turn-flow.hxx`: `TurnFlow::run`, `Outcome`, `FlowDeps`, the
  `followUp*` pending paths (a relay confirmation or reply would ride the existing pending machinery).
- `services/llm/src/feature/llm/services/turn/slots.hxx`: `slots::TextSlots` / `RuleText`, `slots::Filled`,
  `slots::Dispute`, `slots::choose` (the "which one" question).
- `packages/lib/text/src/text/name-match.hxx`: `text_norm::matchName` (exact, one-word, ambiguous, missing). The
  camera, guard, productivity and calendar tools already use it.

### 2.2 Rules family pattern

- `services/llm/src/feature/llm/services/tools/module-command.{hxx,cc}` and `module-command-lexicon.hxx`: a rules
  family that returns a `Command{tool, arguments, fill}` from a lexicon, with `phraseLength` for multi-word phrases.
  The relay family follows the same shape; its lexicon is a static per-language constant, as
  `packages/lib/phrase/src/phrase/details/` requires for vocabulary.

### 2.3 MCP tool runtime

- `packages/lib/mcp/src/mcp/tool.hxx`: `ToolSpec` (`module`, `capability`), `ToolOutcome`, `ToolFailure`.
- `services/llm/src/feature/llm/services/tools/tool-registry.hxx`, `tool-executor.hxx`, `tool-access.{hxx,cc}`:
  `tool_access::holds` and `runnable` answer from the capability table.
- `services/llm/src/feature/llm/services/tools/core-tools.{hxx,cc}`: `coreToolServer(std::vector<tools::ToolDescriptor>)`,
  the in-process server that already holds `app.open` and `app.show_camera`. `message.relay` is served there, as a
  core tool, not through an MCP provider.
- `services/llm/src/shared/vocabulary/tool-contracts.hxx`: `tools::ToolContext` (`userId`, `role`, `lang`,
  `utterance`), `ToolCall`, `ToolResult`, `ToolDescriptor`.
- `services/llm/src/feature/llm/services/tools/reply-claims.hxx`: the guard that stops a spoken claim about an action
  that did not happen. A relay reply must pass it: "se lo dije" is a claim.

### 2.4 Notification delivery

- `services/notification/src/app/rpc/notification-rpc-service.cc`: `CreateNotifications`. Today it admits the caller
  `grpc.caller_llm` only for `type = assistant_task`, exactly one recipient, and no `response` plan. Phase 2 extends
  this admission for the new type (section 4.3).
- `packages/clients/notification/src/notification/notification-client.hxx`: `createNotifications(...)` is already
  the client method argus-llm uses for `assistant_task` (`services/notification/CONTEXT.md`, "argus-llm tells the
  user about a kept request"). The relay uses it; no new client.
- `services/notification/src/feature/notification/`: the notification row and its token (`notification_token`).
- `services/notification/CONTEXT.md`, "The notification change feed (3a-2c)": a row created is an `Add` to the
  recipient's room, through `change_outbox`. The relay adds no sync operation.
- `services/llm/src/feature/pending-intent/`: kept requests with a 5-minute life (the existing default). The relay
  thread's reply window uses the same shape but is its own table (section 3.3); the pending-intent table is not reused,
  because its rows are completed by a module feed, not by a reply.

### 2.5 Capabilities and roles

- `packages/lib/auth/src/auth/capability.hxx`: `kCapabilities` (the table), the masks `kOwnerResident`,
  `kOwnerResidentGuard`, `kEveryRole`, `kEveryRoleBaseline`, `kNonOwnerBaseline`, and `capabilitiesFor`,
  `hasCapability`, `hasAppAction`.
- `packages/lib/auth/src/auth/role-access.hxx`: `kTableAccess` (role to table), `kSessionAccess`, `kRtcAccess`,
  `kSyncAccess`, `kGuardAccess`, `kRouteOverrides`, `kCoreRoutes`. A relay has no table and no HTTP route in phase 2,
  so `kTableAccess` is not touched.
- `packages/lib/auth/src/auth/tool-gate.{hxx,cc}`: `tool_gate::capabilities()` gates an MCP tool call on the same table.

### 2.6 Existing sync paths

- `services/sync/`: `/sync` and the change feed carry the notification row. Nothing new in the sync engine.
- `services/voice/src/feature/voice/voice-session-service.hxx:61`: `bool announce(int64_t userId, const std::string& text)`,
  which speaks into the user's live sessions. This is the spoken leg of the conversation channel (section 6); it is
  not a new path.
- `services/notification/src/feature/call/`: the call engine. Read, not used by the relay.

### 2.7 Module status, stated plainly

- Exists: everything in 2.1 to 2.6 above, at the paths given.
- Does not exist: a relay tool, a relay rules family, a `household_message` notification kind, a `relay_thread`
  table, a `messages.relay` capability, a per-user Conversation store (F9), any camera channel (the watchman).

## 3. What is new, with the rule-23 layout

### 3.1 Folder

All relay code lives under `services/llm/src/feature/messenger/`, a feature of argus-llm, because argus-llm already owns
the router, the tool runtime and the pendings (`services/llm/CONTEXT.md`). The feature needs no second owner:
no other service reads the relay tables (rule 2+ rule of section 23).

```
services/llm/src/feature/messenger/
  CMakeLists.txt
  vocabulary/
    relay-outcome.hxx      RelayOutcome, RelayChannel enums + relayOutcomeToString/FromString pairs
  services/
    relay-command.{hxx,cc}     the rules family: trigger and reply phrases, Command{tool, arguments, fill}
    relay-service.{hxx,cc}     RelayService: resolve the recipient, pick the channel, write the thread
    relay-tool.{hxx,cc}        the ToolSpec and ToolDescriptor for message.relay (served by coreToolServer)
    conversation-port.hxx      interface to F9: open leg check and in-conversation delivery (section 6)
  repositories/
    relay-thread/
      relay-thread-query.hxx   SQL strings and RelayThreadWriteInput / RelayThreadFindInput (global structs)
      relay-thread-repository.{hxx,cc}
  tests/                       (the tests of this unit live in services/llm/tests/unit/, as the others do)
```

The vocabulary (`relay-outcome.hxx`) is used by the service and the repository only, so it stays in the feature and not
in `shared/`. It moves to `shared/` when a second feature reads it (rule 23).

### 3.2 Enums (rule 1)

- `RelayChannel`: `Conversation`, `Notification`, `Camera` (declared now, unused until the watchman, so the wire
  vocabulary is stable), with `relayChannelToString` / `relayChannelFromString`.
- `RelayOutcome`: `Delivered` ("se lo dije"), `Notified` ("le dejé el mensaje"), `NoAnswer` ("no contestó, le dejé el
  mensaje"), `NoRecipient` ("no encuentro a nadie con ese nombre"), `Unavailable`, with the same pair. These are the
  only three outcomes a sender hears about presence, and the enum is the only place they are spelled.

Rule 1 applies to a constrained column. `relay_thread.channel` and `relay_thread.state` are constrained by the
enums in the DDL CHECK, following the schema rule for `notification_token.provider`
(`services/notification/CONTEXT.md`, "Push tokens"): the enum is enforced at the boundary, and the CHECK is written
when the table is created.

### 3.3 Table (rule 26: llm's own schema)

`services/llm/database/schema.sql`, one additive table:

```
relay_thread (
  id INTEGER PRIMARY KEY,
  from_user_id INTEGER NOT NULL,
  to_user_id INTEGER NOT NULL,
  message_id TEXT NOT NULL,       -- the household_message notification id or the conversation turn id
  channel TEXT NOT NULL CHECK (channel IN ('conversation','notification','camera')),
  state TEXT NOT NULL CHECK (state IN ('open','answered','expired')),
  reply_until INTEGER NOT NULL,   -- unix seconds, created_at + messenger.reply_window_s
  created_at INTEGER NOT NULL
)
CREATE INDEX idx_relay_thread_to_open ON relay_thread(to_user_id, state, reply_until);
```

No user table is referenced (rule 27). The index serves the one reply lookup per turn: "does this user have an open
thread from someone?"

### 3.4 Structs (rule 2)

Every function with three or more parameters takes one struct. Names, all in the feature:
`RelayInput` (sender, utterance, lang, roster, now), `RelayResult` (outcome, channel, recipient name), `RelayThreadWriteInput`,
`RelayThreadFindInput`, and `RelayCommandInput` (utterance, lang, roster names).

### 3.5 Errors (rule 6)

Refusals are thrown: `MessengerErrors::RecipientNotFound`, `MessengerErrors::MessageTooLong`, `MessengerErrors::ChannelUnavailable`,
catalogued in `services/llm/src/feature/messenger/vocabulary/messenger-errors.hxx` (one catalog header, as the other
boundaries do). The tool answers a refusal with `tools::ToolResult{.ok = false, .code = ...}`.

### 3.6 Build

`CMakeLists.txt` in the feature folder declares the module with `argus_module`; the parent of
`services/llm/src/feature/` discovers it (rule 25). No consumer edits its `.cc` list.

## 4. Contract changes

### 4.1 The capability and the role rows

| role | `messages.relay` | notes |
|---|---|---|
| Owner | granted | every role may relay to any member (decision 1.5) |
| Resident | granted | |
| Guard | granted | Guard is a household role; its visits and guard calls stay as they are |
| Guest | granted | a Guest is a registered household member; `kCoreRoutes` is unchanged |
| Unknown | not granted | `Unknown` holds no permission anywhere (rule 7) |
| no account (a stranger) | not granted | no tool is offered (decision 1.6) |

The capability row is `{.id = "messages.relay", .module = kCoreModule, .roles = kEveryRoleBaseline}`. It is core, so it is never
gated by a module and never needs `kModuleRoutes`. Rule 7's `kTableAccess` is not changed: a relay writes no table of
the role's own and reads none.

### 4.2 Sync

No new `SyncOperation` value. The relayed message reaches a recipient as a notification row, which is the existing
`Add` through `change_outbox`. The answer is a row too. `ResourceChanged` is not used (its domains are the screens'
reads, and the notification screen already reads `/notification`).

### 4.3 Notifications

A new notification type, `household_message`, is a core kind: it is not in
`services/notification/src/shared/vocabulary/notification-kind.hxx` `kModuleKinds`, so it is created while every module is
off. Its `data` is `{kind: "household_message", fromUserId, threadId}` and nothing else; the text is the body.
`CreateNotifications` admits `grpc.caller_llm` for this type under the same rule as `assistant_task`: exactly one
recipient, no `response` plan (so it can never start a call), and a refusal for any other type. The admission lives in
`notification-rpc-service.cc` and is pinned by `notification-rpc-test`.

### 4.4 Tools and the capability list

- The tool `message.relay` (`{"recipient": text, "message": text}`) is offered only to a holder of `messages.relay`
  (tool_access). Its `annotations.destructive` is false, `openWorld` false.
- `InitialInfo.info.context.capabilities[]` gains `messages.relay` for every role above, because the list is built
  from `capabilitiesFor`. The app reads it; no new field.

### 4.5 Frontend coordination

The backend change is additive. The frontend coordination note goes to the `frontend/` owner and asks for:

1. The `messages.relay` capability string in the app's capability reader (no UI yet).
2. The `household_message` notification type rendered like the existing notification rows (title, body, read state).
3. No screen in phase 2. A messages screen, if wanted, is a later frontend unit and needs a `ResourceChanged` domain,
   which is not registered in `packages/contracts/sync/src/sync/resource-change.hxx` today.

## 5. Measurements: phase-2 tests and benchmarks

The parallel-conversations plan defines P1 to P5 (section 7), and none of them is a messenger measure. The messenger's
own measures are named MS1 to MS6 below. The camera watchman plan uses MS1 to MS6 for its measures, so the two sets never
share a name.

| id | measure | test or benchmark | threshold | knob |
|---|---|---|---|---|
| MS1 | rule recall on relay phrases | `services/llm/tests/unit/` fixture of trigger and reply phrases, Spanish first, English second | recall >= 0.95 on the fixture | none (the lexicon is a constant) |
| MS2 | false relay on other turns | the same fixture plus the existing turn fixtures (`services/llm/tests/fixtures/eval/cases.jsonl`) | zero false relays on non-relay turns | none |
| MS3 | recipient resolution | roster fixture with one-word, ambiguous and missing names, through `text_norm::matchName` | exact match on one-word names; ambiguous and missing names always ask | `messenger.match_min_words` default 1 |
| MS4 | channel choice and the no-ring rule | unit test with the call engine mocked: no `CallIncoming`, no `CallService` call, one notification row | no call from a relay, ever | none |
| MS5 | conversation delivery latency | benchmark of the port with F9 stubbed | under 100 ms from takeover to speak, as set by the parallel plan | none |
| MS6 | payload privacy | unit test: the relay payload contains the sender's utterance slot and no memory fact, no summary, no context block | byte-equal to the expected payload | none |

Benchmarks are run in phase 2 only with the heavy-gate wrapper that the RAM rule requires; this note does not ask for
any run now.

Config knobs, with defaults (`services/llm/config.toml.example`, section `[messenger]`):

```
[messenger]
enabled = true
reply_window_s = 300
max_message_chars = 500
match_min_words = 1
```

`enabled = false` removes the tool from `offered` and the rules family from the rules decider. A relay written before
disabling stays readable.

## 6. The interface with the parallel-conversations track

The track `track/b4-conversations` (F9) is writing `docs/design/parallel-conversations.md`. As of this note that file is not on
its branch. The shared points, named here so both notes agree:

1. **The router.** `RuleDecider` and `RouterDecider` are shared. F9 adds per-user state to `DecideInput` (the
   conversation's open leg). This note adds no field to `DecideInput`; it reads `offered` and `modules` only.
2. **The per-user Conversation.** F9 owns it (keyed by `userId`, argus-llm). Phase 2 needs exactly one read from it:
   `ConversationPort::openLeg(userId) -> optional<leg>` and one write: `ConversationPort::deliver(userId, text) -> bool`.
   Both are declared in `services/llm/src/feature/messenger/services/conversation-port.hxx`. Until F9 lands, the port
   has one implementation, which answers "no open leg" and "not delivered". That keeps phase 2 whole.
3. **Delivery of a message into a conversation.** The spoken leg is the voice path `announce(userId, text)`
   (voice-session-service.hxx:61). F9's leg change decides which session hears it. Phase 2 calls `announce` only through
   `deliver`.
4. **Memory scope.** F9 section 5 adds `personal` and `household` scopes to memory facts. Today `memory.scope` holds
   only `'user'` (`services/llm/src/feature/memory/repositories/memory-graph/memory-graph-query.hxx`). The relay reads no
   memory, so phase 2 does not depend on the scope change; it depends on it only for MS6's "no fact" rule, which holds
   either way.
5. **Pendings.** F9's per-person pendings and the relay's reply window share the 5-minute default and the
   `TurnFlow::followUp*` shape. Neither reuses the other's table.

## 7. Privacy and risks

- **Strangers.** A stranger is not a role, has no `JwtContext`, and is offered no tool (decision 1.6). The visitor
  path reads the roster only to match the asked name, and returns one line whatever the match: the three cases and
  the "learns nothing" rule are pinned by `llm-messenger-service-test`.
- **Household memory.** A relayed message is the sender's words only (decision 1.8). The memory graph is one graph
  today (`scope = 'user'`); F9's scopes keep a personal fact of one member out of another member's conversation. The
  relay reads neither, so the messenger cannot leak memory; the risk is the sender's utterance itself, which the sender
  chose to say to the recipient. That is stated to the sender in the first relay of a session ("le digo a David tal
  cual"), once, and not as a warning on every message.
- **No information leaks.** The sender hears only the three outcomes of decision 1.7. The recipient's presence,
  location, and whether they are home are never in the relay result, the log line, or the notification body.
  Logs record the outcome enum, the channel enum and a hash of the recipient id, never the message text.
- **Disturbance.** Decision 1.3 (no ring) is the rule that prevents a relay from becoming a call. It is enforced by
  MS4, not by convention.
- **Abuse.** A member can send many messages. Phase 2 has no rate limit; a default of 20 messages per sender per hour
  is proposed (section 8, Q11) and not built until ruled.
- **Retention.** A relayed message is a notification row and follows the notification retention. The `relay_thread`
  row expires with its reply window and is deleted after 24 hours by the existing sweep style; no separate
  retention knob.
- **Wrong recipient.** An ambiguous name asks (decision 1.2). A confident wrong match on a one-word name is the
  known risk: MS3 holds it to zero on the fixture, not in the field.
- **Spoken claims.** "Se lo dije" is a claim about an action. The reply guard (`reply-claims.hxx`) must find it
  supported by the relay result before it is spoken.

## 8. Open questions

Each question has a recommended default. Phase 2 code follows the defaults unless the reviewer rules otherwise, and
only the units not blocked by a question are written before the ruling (section 9).

1. **Capability name.** Is `messages.relay` the name, or should it be `household.message`? Default: `messages.relay`
   (the verb names the tool's action, as `guard.mode.set` and `camera.talk` do).
2. **Roles.** Should Guest relay to the household? Default: yes, all four registered roles (decision 1.5), because the
   owner's word was "every registered household member".
3. **Notification type.** New core kind `household_message`, or reuse `assistant_task` with `data.kind`? Default:
   the new kind, admitted under the `assistant_task` rule (section 4.3). Reusing `assistant_task` would also make the
   relay's answer look like a pending notice, which is the wrong user meaning.
4. **Laya label.** Does the relay get a `message_relay` label in the next Laya bundle, or does it run on the rules
   tier alone until then? Default: rules tier alone in phase 2; the label is added with the next bundle, because the
   label set is the only coupling and retraining is outside this repo.
5. **Conversation port before F9.** Ship the port with a "no open leg" implementation until F9 lands, or wait?
   Default: ship it now (section 6.2), so the relay's channel order is complete and tested; F9 replaces one class.
6. **Camera channel.** Empty in phase 2. Default: yes, empty; the enum value is declared so the wire is stable.
7. **Reply window.** How long may a reply come back? Default: 300 seconds (`messenger.reply_window_s`), the same as
   the existing pending life.
8. **Ambiguous or missing recipient.** Ask one question, or refuse? Default: ask one question through `slots::choose`,
   and refuse with `NoRecipient` after the answer does not match.
9. **Strangers.** Phase 2 gives a visitor nothing, not even "no encuentro a nadie"? Default: the visitor hears a neutral
   line that does not mention the roster and does not notify anyone (the resident notification is the watchman's).
10. **A relay to a member who is in a call with Argus.** Deliver inside the conversation, or as a notification?
    Default: inside the conversation through the port once F9 exists; until then, as a notification.
11. **Rate limit.** Default: 20 relays per sender per hour, a config knob `messenger.max_relays_per_hour`, enforced
    by counting `relay_thread` rows. Not built until ruled.
12. **Retention of the relay thread row.** Default: deleted 24 hours after `reply_until`, by the existing sweep style.
13. **Frontend.** Does the app need a messages screen in this programme, or is the notification row enough for phase 2?
    Default: the notification row is enough; the screen is a later unit (section 4.5).

## 9. Phase 2 plan (the units, and what waits for a ruling)

Each unit is its own commit: English subject, a `Report:` line, explicit `git add`, no attribution footer. Tests are
written with each unit. No unit compiles or runs ctest in this phase (coordinator's instruction); the checks run are
`scripts/check-comments.sh`, `scripts/check-deps.sh` and `scripts/check-routes.sh`.

Not blocked by an open question (written now): the enums and their string pairs (section 3.2), the relay rules family
(2.2, default Q4 is rules-only), the recipient resolution through `matchName` (1.2, MS3), the `relay_thread` schema and
repository (3.3, Q7 and Q12 are knobs), and the conversation port with its "no open leg" implementation (Q5).

Blocked until the reviewer rules: the capability row in `capability.hxx` (Q1, Q2), the `household_message` kind and its
admission in `notification-rpc-service.cc` (Q3), the relay tool's offering through `coreToolServer` (depends on Q1 for the capability
string), and any frontend change (Q13).

## 10. What this note could not verify

- The plan's measures are P1 to P5; the MS1 to MS6 of this note are new (section 5). Reviewers should not look for
  messenger MS1 to MS6 in the parallel-conversations plan.
- `kEveryRole` is verified in `role-access.hxx`: Owner, Resident, Guard and Guest, never Unknown. The row uses
  `kEveryRoleBaseline` (section 4.1, ruling 1), so Guard keeps the capability while surveillance is off.
- `packages/clients/notification` exposes `createNotifications`, read in the header only; the call path from argus-llm
  for `assistant_task` was not traced end to end. Verify before the admission change (blocks Q3).
- `announce(userId, text)` exists in `voice-session-service.hxx`; its gRPC or in-process caller from argus-llm was not
  traced. The conversation port's real implementation depends on it (blocks section 6.3, not phase 2's stub).
- The Laya bundle's label set beyond `kLabelTools` and the training repository were not read (`intent-training/` is
  outside this repo). Q4 depends on that.
- `services/llm/tests/fixtures/eval/cases.jsonl` was named but not counted; MS2's fixture size is unknown.
- F9's `Conversation` and the track's note `docs/design/parallel-conversations.md` do not exist on
  `track/b4-conversations` yet, so section 6 names the interface the track must match and does not verify the track's
  choices.
- `reply-claims.hxx` was named for the claim guard; its rule set for "se lo dije" was not read.

## 11. Rulings of 2026-10-14 and what was built

Backend-c6 approved the note at `7aa9dddd` and `a2b1fec6` with these rulings. Each is implemented on the branch
`track/b4-messenger` after the note; section 9 is superseded by this list.

1. Capability `messages.relay`, core, granted through the baseline (`kEveryRoleBaseline`), so a Guard keeps it while its
   module is off. Built: `capability.hxx` and its test.
2. Four registered roles (Owner, Resident, Guard, Guest), never Unknown, never visitors. Built: the same row.
3. A core kind `household_message`, not gated by any module. Built: the admission for argus-llm in
   `notification-rpc-service.cc` (one recipient, no response plan) and its rpc test; the kind is absent from
   `kModuleKinds`, so `moduleOf` is empty.
4. The rules tier alone; no Laya label. Built: `RuleDecider` offers the relay family when `message.relay` is offered.
5. `NoConversation` ships now; the real Conversation comes in an integration commit. The port stays narrow: `hasOpenLeg`
   and `deliver`. Built: `conversation-port.hxx`.
6. The camera channel enum is declared and unused until the watchman is wired in the integration.
7. Reply window 300 s. Built: `messenger.reply_window_s` in `LlmConfig::resolveMessenger`, default 300.
8. An ambiguous or missing recipient asks one question; a second miss is `NoRecipient`. Built: `RelayService::relay`.
9. Strangers (changed by the ruling; decision 1.6). A visitor's ask gets one neutral line, identical whether the name
   matched or not. A matched member is notified with `household_message` from "un visitante en <cámara>"; the visitor's
   answer is the body. No match, or several, notifies nobody. Tests cover the three cases and the "learns nothing"
   rule. Built: `RelayService::visitorAsk`.
10. Conversation before notice, accepted as proposed. Built: `RelayService::deliverTo`.
11. Rate limit: 20 relays per hour per sender, `messenger.max_relays_per_hour`, refused with its reason
   (`MessengerErrors::RateLimited`, 429). Built: `RelayService::admit`, `RelayThreadRepository::countFrom`, and the
   test in `llm-messenger-service-test`.
12. Thread retention and the delivered-or-notified outcomes, accepted as proposed.
13. No frontend screen in phase 2; the notification row is enough.

Second rulings of 2026-10-14 (d and the additions), built after the first list:
- (d) A reply without a name is a reply only with exactly one open thread; two or more ask "¿a quién?" among the
  senders; none asks who as a new message. Built: `RelayService::reply`, the turn's `Awaiting::Recipient`.
- A visitor's notices are limited to `messenger.max_visitor_notices_per_hour_per_camera` (default 6) per camera and per
  member, refused with the reason (`MessengerErrors::VisitorLimited`) and logged; the visitor hears the same line.
  Built: `visitor_notice` table, `RelayService::admitVisitorNotice`.
- Composition: the household roster over `argus::clients::identity` (`IdentityHouseholdRoster`, active members with a
  known role), `message.relay` in `coreToolServer`, the RelayService built by `composeMessenger` with the config of
  `LlmConfig::resolveMessenger`, and the turn's question step. Built, see the commits after `977423d8`.

Third rulings of 2026-10-14, built after the second list:
- Relay storage goes through the graph's own mutex and connection (`GraphMessengerStore`, `MessengerStore::run`): the
  relay tables live in llm.db next to the graph and no second writing connection exists. A concurrency test runs relays
  and graph writes on two threads over one file, with no failed write and an intact database.
- `visitor_notice` is purged with `relay_thread`: one sweep (`RelayService::sweep`) expires overdue threads and purges
  settled threads and notices older than 24 hours, hourly on the loop. The visitor purge is indexed by its time.
- A reply without a name is one message to a sender whose several open threads are the only ones, closing them all.
- `ListUsers` runs without a cache. Static reading: identity's example config lists an `llm` caller under
  `[rpc.callers]`, and `ListUsers` has an empty allowlist, so any credentialed caller is admitted. The deployed
  credential is verified only against a running identity, which is still to be done.

Still open, for the integration commit:
- The real Conversation (F9) replaces `NoConversation`, and the watchman's camera channel is wired to `RelayChannel::Camera`.
- Visitor notices carry `fromUserId` 0 and no thread row, so a member's reply cannot travel back to a visitor.
- The rate-limit count and the insert are two locked steps, so two concurrent relays can both pass a limit of one
  below the cap; a single locked step would close it.

## 12. Integration with the conversations (T4, 2026-10-19)

The integration commit of track T4 lands the open items of section 11 this way:

- **The courier replaces the notifier and the open-leg port.** `RelayService` hands every letter to a
  `messenger::MessageCourier`; the production courier is argus-notification's `ConversationService.RelayMessage`
  (`NotificationMessageCourier`). argus-notification stores one copy of the message for the sender and one for the
  recipient (`conversation_message`, synced to the app), says it inside the recipient's live session through
  argus-voice's `Announce` (`delivered`, "se lo dije"), or leaves it as a `household_message` notification
  (`notified`, "le dejé el mensaje"). argus-llm never rings anyone and never learns where the recipient is: the
  receipt names only the channel.
- **The conversation context.** When the message was said in the recipient's live session, the line is also noted
  in the recipient's per-person conversation (`ConversationService::appendInbound`), so a later "dile que ya voy" is
  understood in context.
- **Consent.** `message.relay` is a tool with a spoken confirmation: the first call resolves the recipient without
  side effects and reads the exact words back ("¿Le digo a David: «venga a comer»?"); only the sender's yes in the
  next turn, with the one-use code of that recipient and those words, sends it. The tool is annotated destructive so
  the grounding layer enforces the yes in a later turn.
- **No impersonation.** The refusal leg lets an offered relay through to the rules tier, and refuses, offered or not,
  a relay that asks to speak as someone else or to hide the sender ("de parte de", "en nombre de", "hazte pasar
  por", "anónimo", "on behalf of", "pretend to be", "anonymously"). A message always names its sender.
- **Answers and expiry.** A reply names the message it answers (`reply_to_key`), so both copies of the original move
  to `answered`; the hourly sweep expires unanswered threads and settles each message as `expired` in the inbox.
- **Retention.** Every inbox row carries `expires_at` from the owner's `conversation.retention_days`; argus-notification
  removes expired rows hourly and sends their tombstones.
- Still open: visitor notices land in the member's Argus thread but cannot be answered back to the visitor; a message
  typed in the app is not part of this unit (the app reads and clears threads, messages are sent by voice).
