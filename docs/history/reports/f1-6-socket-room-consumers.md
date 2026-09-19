# Phase 1 step 6 — `socket` and `room`: the step is its claim

Scope: the row says there is **nothing to do here** for `socket` and `room`, and gives
a reason — "their only consumers are the gateway and each other (§9.1), so they die in
Phase 3a with `services/sync`". A step whose whole content is a claim is verified by
measuring the claim. Both halves of the reason are false, and the corrected consumer
list is what Phase 3a inherits; the decision to defer the deletion is unaffected.
Base `2b08c3f`.

## What was measured

Consumers are counted from the code that names their headers, not from `CMakeLists`
link lines: a project can link a target it never uses (the guarded-subtree pattern) and
can use a header it reaches transitively.

**`packages/room`** is two files, `room-manager.{hxx,cc}`; the header is included by
four parties:

| consumer | where |
|---|---|
| `packages/socket` | `shared/services/socket/socket-service.hxx:7`, `…/sync-change.hxx:6` |
| `packages/sync` | `src/feature/socket/sync/services/sync-service.hxx:15` |
| `services/gateway` | `src/main.cc:19`, `src/sync/sync-fan-out.{hxx:6,cc:5}`, `tests/audit-sync-read-test.cc:19`, `tests/gateway-test.cc:15` |
| `services/camera` | `src/main.cc:34` |

`packages/socket/CMakeLists.txt:61` links `argus::room` and `:36` guards it;
`packages/sync:182`, `services/camera:266,340` and `services/gateway:192` link it too.

**`packages/socket`** exports four things and all four are spoken outside the gateway:

| what | consumers |
|---|---|
| `contracts/user-change-sink.hxx` | `notification` (`src/main.cc:17`, `nats-notification-change-sink.hxx:5`, three test files), `productivity` (`src/main.cc:14`, the NATS sink, five feature services, its controller test), `packages/sync` (`shared/services/notification/notification-service.hxx:7`) |
| `contracts/camera-change-sink.hxx` | `camera` (`feature/api/{camera,zone}/services/*-feature-service.hxx:8`, `camera/nats-camera-change-sink.hxx:5`) |
| `dtos/socket-emit/socket-emit-dto.hxx` | `gateway` (`sync/notification-delivery-consumer.cc:6`, `sync/sync-fan-out.hxx:5`), `identity` (`feature/rpc/identity-rpc.cc:10`, `feature/api/user/services/user-feature-service.cc:6`, `feature/api/invitation/services/invitation-feature-service.cc:13`), `packages/sync` (`feature/socket/sync/services/sync-service.cc:7`, `…/synchronized-service.hxx:13`) |
| `services/socket/socket-service.hxx` | `identity` (`user-feature-service.hxx:7`, `invitation-feature-service.hxx:8`, `auth-service.hxx:21`), `audit` (`shared/services/audit-log/audit-log-service.hxx:7`, `…/user-audit-log/user-audit-log-service.hxx:7`) |

`services/socket/sync-change.hxx` is included by `gateway` (`notification-delivery-consumer.cc:8`,
`sync-fan-out.cc:6`, `gateway-test.cc:16`), `notification` (`nats-notification-change-sink.cc:5`),
`productivity` (its NATS sink, `:5`), `identity` (`identity-rpc.cc:12`) and the package's own
suite. Targets linking `argus::socket`: `audit:56`, `sync:181`, `camera:265,341`,
`notification:210,237,314,329`, `productivity:221,254`, `identity:230`, `gateway:192`.

So the row's "the gateway and each other" names 2 of 8 parties. The other six —
`camera`, `notification`, `productivity`, `identity`, `audit` and `packages/sync` — are
the producers whose outboxes and transports §9.1's `socket` row is about.

## Why the decision still holds

The row's conclusion (delete nothing in Phase 1) survives its own premise failing,
because §9.1's `socket` row already says where the package's three jobs go — payload
vocabulary to `contracts/sync`, transport and fan-out to `services/sync`, and
`lib/nats` plus each producer's own outbox (§3.6) — and that split needs `services/sync`
to exist, which is Phase 3a's work, itself gated on the gateway dying (D5). Deleting
either package now would break six projects that still need the vocabulary.

The correction changes **Phase 3a's scope, not Phase 1's**: the phase does not only have
to lift `socket`'s internals into `services/sync`, it has to repoint the seven
consumers above at the vocabulary's new home in `contracts/sync` — `identity`, `audit`
and the four services name the emit DTO and the sink contracts directly, and
`packages/sync`'s `feature/socket/` names both packages. `room` is smaller but not
free either: beyond `socket` and the gateway, `sync-service.hxx` and `camera/src/main.cc`
include `room-manager.hxx` today, which §9.1's `room` row denies.

## What was changed

- **Plan row 6** now records the measurement instead of the claim: nothing deleted, the
  consumer list above, and the note that Phase 3a's repointing set is seven consumers,
  not one.
- **§9.1's `room` row** said "its only consumer is `socket` (`socket-service.hxx`
  includes `room-manager.hxx`)" — corrected to the four consumers measured here;
  `packages/sync`'s `sync-service.hxx:15` and `services/camera/src/main.cc:34` are the
  two that make it false. The row's file count (2) and its verdict (dies in Phase 3a)
  are right and stay.

## Verification

- **No source or build file is touched**: the step's diff is markdown only
  (`git diff --name-only` → row 6 of the plan, §9.1, and this report), so the phase
  gate is not re-run: `./scripts/build-all.sh dev` was green at `2b08c3f`, which is this
  same C++ tree, 18/18 projects and 288 reached tests.
- **The consumer list is reproducible**: `grep -rn --include='*.cc' --include='*.hxx'
  -E '#include [<"][^">]*room/' services packages` and the same for `socket-service`,
  `sync-change`, `socket-emit` and the two sink contracts, excluding `build/`.

## Findings outside this unit

- **Three projects guard the two subtrees without linking them or naming either**:
  `services/guard` (`CMakeLists.txt:96,100`), `packages/cert` (`:45,58`) and
  `packages/memory` (`:120,124`). `memory` documents why its guard is live —
  `CMakeLists.txt:113` says its own subtree "reaches argus::socket for its change sink"
  through `audit`, and a guarded subtree only needs the *target* to exist for the
  standalone configure — but `cert` and `guard` neither link the aliases nor name a
  header from either package anywhere in their sources, which would make those four
  guards leftovers of the scripted guard insertion (step 2). Left alone: a dead guard is
  a configure-time no-op until the aliases disappear, and removing provably dead guards
  belongs with the step that unifies the standalone-block sentinels (already deferred in
  row 2's entry).
