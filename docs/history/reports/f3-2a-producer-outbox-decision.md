# F3-2a — the producer outbox: measurement and decision

Sub-step 3a-2 of `docs/history/plans/architecture-plan.md:832` asks that the
producers (`camera`, `guard`, `notification`, `productivity`, `identity`) move
to `contracts/sync` + `lib/nats` + **their own durable outbox with
fingerprints**, that `sync` become the single audit writer, and that `identity`
move its six `user_action_log` writes onto the additive action subject
declared as a new row in `wire-nats-subjects.md`.

This report is the sub-step's first artefact and it is a measurement before it
is a plan: of the row's three clauses, **two are already landed** and the third
rests on a premise about the existing code that **measurement contradicts**.
Everything below was read at `dfbaf99`; every claim carries its file and line.

## 1. Already landed, and where

**The producers already speak the contract and the bus.** All four real
producers link `argus::contracts::sync` and `argus::lib::nats` —
`services/camera/CMakeLists.txt:251,256`, `services/notification/CMakeLists.txt:136,137`,
`services/productivity/CMakeLists.txt:143,144`, `packages/identity/CMakeLists.txt:207,213`
— and each installs its own sink at boot: `services/camera/src/main.cc:158`,
`services/notification/src/main.cc:157`,
`services/productivity/src/main.cc:130`,
`services/gateway/src/main.cc:327` (the host of `packages/identity`).

**`sync` is already the single audit writer.** `user_action_log` has exactly one
`INSERT`, in `services/sync/src/shared/repositories/user-action-log/user-action-log-query.hxx:11-13`,
and its only caller is `AuditFanOut::insertAction`
(`services/sync/src/feature/fanout/services/audit-fan-out.cc:87-97`). No
`INSERT INTO user_action_log` exists anywhere else in the tree, and
`packages/identity` retains no repository, service or statement against the
table.

**`identity`'s action journal already travels the additive subject.** The six
writes are six `publishAction` calls on `IdentityChangeSink`
(`packages/contracts/sync/src/sync/identity-change-sink.hxx:41`), published by
`NatsIdentityChangeSink::publishAction`
(`packages/identity/src/feature/api/user/services/nats-identity-change-sink.cc:104-112`)
to `nats_subject::kIdentityUserAction = "argus.identity.v1.user-action"`
(`packages/lib/nats/src/nats/nats-subject.hxx:30-31`), consumed by
`sync_fan_out::subscribeActionJournal`
(`services/sync/src/feature/fanout/services/sync-fan-out.cc:153-162`) and
inserted verbatim. The subject already has its row and its payload section in
`docs/architecture/wire-nats-subjects.md`, and `UserActionLog = 20` is already
served in the `Synchronize` page for Owner
(`services/sync/src/feature/transport/services/synchronized-service.cc:235`,
`:59-60`). **The row's second half is done**; it landed in F3-1b and F3-1c.

**`lib/nats` already has the JetStream leg the row asks for.** `NatsBus::publishWithMsgId`
publishes through `js_Publish` with `MsgId` and a 2000 ms `MaxWait`, returning
false when the broker did not store the message and **never degrading to core
NATS** (`packages/lib/nats/src/nats/nats-bus.cc:462-491`, contract at
`nats-bus.hxx:54-57`); `ensureStream` creates or reconciles the stream
(`:493-530`). The package therefore needs **no change** for this step — the
gap is entirely in the producers.

**`guard` is not a change producer, and already owns the pattern.** It links no
`argus::contracts::sync`, includes no `<sync/…>` header, and has no
`Nats*ChangeSink`; its two NATS publishes are
`argus.guard.v1.heartbeat` (fire-and-forget,
`services/guard/src/feature/guard/guard-service.cc:910`) and
`argus.guard.v1.encounter_closed`, which is drained from `guard_encounter_outbox`
with `publishWithMsgId(msgId = eventId)` and settled by `markEncounterSent`
(`guard-service.cc:974-991`). Its enqueue is **inside the domain transaction**
(`guard-repository.cc:673-679`). The plan names `guard` as a producer to
migrate; measured, its row is already the shape this step wants.

## 2. What is not landed: 36 call sites, all fire-and-forget

Four producers reach a change sink today, at 36 call sites — camera 4,
notification 1, productivity 14, identity 17 — and every one of them publishes
**straight to the bus with no durable row behind it**. The sink bodies are
`bus_->publish(subject, json)`: `nats-camera-change-sink.cc:16-21` and `:23-45`,
`nats-notification-change-sink.cc:36-68`,
`nats-productivity-change-sink.cc:36-68`,
`nats-identity-change-sink.cc:19-112`. A failed publish is a `LOG_WARN` and the
change is gone; the void `emitModule`/`emitUser`/`emitUsers` overrides do not
even read `publish()`'s return value
(`nats-camera-change-sink.cc:19-20` sets `(void)table;` and discards the bool).
No producer wraps its publish in a database row, so a restart between the
domain write and the publish loses the event permanently.

That is the whole of the remaining work: **four durable outboxes**.

## 3. The premise that does not hold

`docs/history/plans/architecture-plan.md:473-479` claims the pattern "the
repository already uses" is `object_event_outbox`, `guard_action_outbox` and
`notification_command` + inbox receipts. Measured, one of the three is not a
NATS outbox at all, and the fingerprint rule the plan states lives on the
**consumer** side, not on any producer table it names.

| Cited as the precedent | What it is | Publishes | PubAck | `fingerprint` | Conflict rule |
|---|---|---|---|---|---|
| `object_event_outbox` (`services/camera/database/schema.sql:134-147`) | real producer outbox | yes | **yes** | **no** | **none** |
| `guard_action_outbox` (`services/guard/database/schema.sql:182-203`) | gRPC effect ledger | **no** | n/a | no | payload CAS |
| `notification_command` (`services/notification/database/schema.sql:53-59`) | sender idempotency | no | n/a | **yes** | **yes** |
| `guard_encounter_outbox` (**not cited**, `services/guard/database/schema.sql:205-217`) | guard's real NATS outbox | yes | **yes** | no | none |
| `notification_delivery_inbox` (**sync**) | inbox receipt | consumer | broker | **yes** | **yes** |
| `encounter_closed_inbox` (**memory**) | inbox receipt | consumer | broker | **yes** | **yes** |

- **`guard_action_outbox` has no `event_id` column** and no `SELECT` by status
  anywhere in the service: it is never drained. Its fourteen columns record the
  outcome of an awaited gRPC camera command — `in_flight`, `succeeded`,
  `rejected` — and its `attempts`/`next_attempt_at` drive `retryBackoffAt`,
  not a publisher.
- **`object_event_outbox` carries no fingerprint**, and the id it is keyed by is
  *not* deterministic: `cameraId:nowMs:sequence`
  (`services/camera/src/operator/nats-object-event-sink.cc:63-66`). Same id
  with a different payload is swallowed by `INSERT OR IGNORE` as a duplicate
  with no conflict path at all.
- The plan's sentence verbatim — "same id with the same fingerprint is a
  replay, same id with a different fingerprint is a conflict that is never
  dispatched" — is written into exactly two schemas, both **inbox** side:
  `services/sync/database/schema.sql:61-63` (`notification_delivery_inbox`) and
  `packages/memory/database/schema.sql:134-143` (`encounter_closed_inbox`).
  The frozen prose scopes it correctly
  (`docs/architecture/wire-nats-subjects.md:33`); the plan does not.
- `docs/architecture/data-storage.md:49-53` propagates the same error: it
  presents `object_event_outbox` and guard's three tables together as "keyed by
  `eventId` and by deterministic `commandId`". `guard_action_outbox` has no
  `event_id` column, and only `guard_encounter_outbox` publishes.

**No single existing table does all of what the row asks.** What the repo
demonstrates is a composition, and each piece is proven: the structure, drain
thread, overflow policy and PubAck of `object_event_outbox`; the in-transaction
enqueue of `guard_encounter_outbox`; the fingerprint, replay/conflict
disposition and refusal of `notification_command` (`ALREADY_EXISTS` at
`services/notification/src/feature/rpc/notification-rpc-service.cc:191-195`,
SHA-256 over a length-prefixed canonical form at
`notification-repository.cc:80-141`); and the canonical-JSON hash of the two
inboxes (`argus::hash::sha256Hex(json_util::toString(json_util::fromString(p)))`,
`services/sync/src/feature/fanout/repositories/delivery-inbox/delivery-inbox-repository.cc:14-18`).

## 4. The design the measurement supports

Two readings of "a deterministic fingerprint per id" are available, and they
are not equivalent for a change feed, because a change feed's events are
**transitions of a mutable row** while every existing precedent's id names a
**row that is written once** (`command_id` from the client; `delivery_id`; an
encounter's closed state).

**(A) Transition-addressed, for the change events.** `id = sha256(table |
recordId | canonical(before))` and `fingerprint = sha256(canonical(diff))`,
with the diff produced by `JsonDiff::createFlatDiff`, which stays in the
producer (§3.5). This reading makes the plan's sentence mean something: a retry
of the same logical mutation re-reads the same `before`, so it re-derives the
same id and the same diff and is a **replay**; a later mutation of the same
record has a different `before` and therefore a different id, so it is a
**different event and is dispatched** — the failure mode a per-record id would
have (silently dropping every change but the first) cannot occur; and two
writers racing from one prior state with different `after` snapshots collide on
the id with different fingerprints, which is a **conflict that is never
dispatched**.

**(B) Outbox-row-addressed, for the action journal.** A `read` changes no row,
so `before` is empty and (A) would collapse two portrait views into one — a
lost audit row. The journal's id is therefore the outbox row's own, carried in
the row and in the JetStream `MsgId` (the shape `notification_delivery` already
uses: `"notification-delivery:" + deliveryId`), and its fingerprint is the
canonical payload hash. A redelivery of that row is the replay; the conflict
case is structural.

**What makes either safe on the consumer side is already measured.** `sync`
absorbs a redelivered change: `AuditLogService::create` merges same-record,
same-day diffs through `JsonDiff::compareChanges` and re-inserts
(`services/sync/src/feature/fanout/services/audit-log-service.cc:28-64`), so an
outbox replay cannot double-count an audit row. The journal leg is insert-only
and needs no merge.

**The drain shape** is `object_event_outbox`'s: enqueue in the domain
transaction, a dedicated drain that publishes one pending row at a time with
`publishWithMsgId(msgId = event id)`, marks sent **only on PubAck**, and records
an attempt otherwise — with the status-guarded CAS that camera has and
`guard_encounter_outbox` lacks.

### Amendment, recorded while 3a-2b was implemented

Reading (A) discriminates the id by the transition's **pre-state**
(`canonical(before)`). The camera producer, the template the other three copy,
implemented the sharper form: the discriminator is the transition's **own
payload** — the emitted `{operation, option, info}` frame for a change and the
audit input for a journal row — so
`id = sha256(table | recordId | canonical(payload))`.

The reason is a trap measured in review rather than reasoned about. With a
pre-state discriminator a record that goes a→b→a→b re-derives the *first*
event's id on the second a→b, and the payload it arrives with is a new one (the
audit frame carries a millisecond timestamp), so §3.5's rule makes it a
`Conflict` that is never dispatched: the client keeps the first revision for
ever. Payload discrimination keeps every property (A) was chosen for — a
replayed transition re-derives the same id and is a replay, two writers racing
from one prior state with different frames collide on the id with different
payloads — and adds the one it lacked: every transition of a record is a
distinct event, including a return to a state it already held.

## 5. Decomposition

One unit per producer, per the standing rule that a unit of work is one package
or one microservice:

| Sub-step | Unit | Call sites | Note |
|---|---|---|---|
| 3a-2b | `services/camera` | 4 | owns the closest existing pattern (`object_event_outbox`) |
| 3a-2c | `services/notification` | 1 | smallest; its two `emit*` overrides are dead |
| 3a-2d | `services/productivity` | 14 | |
| 3a-2e | `packages/identity` | 17 | plus (B) for the six journal writes |
| 3a-2f | `services/sync` | — | only if a producer's replay needs a receipt the merge cannot absorb; measured today, it does not |

## 6. Open question, recorded rather than assumed

The fork in §4 is a decision about durability semantics that the plan's author
should own, because it is the one place this sub-step does not follow the plan
literally: reading (A) makes the plan's conflict rule real but derives the id
from the transition's own discriminator instead of from a client-supplied
command id. The implementation proceeds on (A)+(B), which is the reading the
plan's own words support for the change feed and the only one under which the
journal keeps every occurrence; if a different reading is wanted, the change is
confined to the first producer's outbox, which is the template the other three
copy. The amendment above narrows (A)'s discriminator from the pre-state to the
payload; it does not change that choice.
