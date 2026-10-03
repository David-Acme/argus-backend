# argus-nats

The NATS event bus every service publishes and consumes through: one wrapper
over cnats, the subject vocabulary, and the push-intent sink the publishers
install at boot.

## What this is

A PACKAGE, not a service: no route, no `main`, no database. Ten units
outside `packages/lib` link it — `contracts/sync` and the nine services that
publish or consume the change stream (auth, camera, guard, identity, llm,
notification, productivity, sync, tunnel) — because the change stream is how a
domain tells the others that a row moved.

`docs/architecture/wire-nats-subjects.md` is the contract this package
implements: every published subject and stream constant here appears there,
and a subject that is not in that document is a subject nobody consumes. The
one constant with no row is `kGuardSubjectFilter` — a `>` subscription filter
argus-guard binds its own feed with, not a spelling anything publishes on.

## Layout

- `src/nats/nats-bus.{cc,hxx}` — `NatsBus`: connect, publish, subscribe,
  unsubscribe, the wildcard delivery that hands the handler the concrete
  subject it matched. Handlers run on cnats worker threads and must not block.
- `src/nats/nats-subject.hxx` — the subject vocabulary:
  `argus.<domain>.v1.change` for the change streams, the guard and camera
  event subjects, the notification delivery stream and the push-intent
  subject.
- `src/nats/push-intent-sink.hxx` — `PushIntent` and the `PushIntentSink`
  interface: display-only, mirrored to the device through the tunnel, never a
  `/sync` event.
- `src/nats/nats-push-intent-sink.hxx` — the publisher both intent producers
  install, over the bus and `push_intent::toJson`.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME nats ...)`
  declaration, explicit source lists, never `file(GLOB)`. The include root is
  `src/`, so consumers write `<nats/nats-bus.hxx>`.
- A subject is declared once, here, and consumed as a constant. A string
  literal for a subject is the duplication that breaks the stream the first
  time the two copies disagree, and it hides the subject from the wire
  document.
- A core publish is fire-and-forget by design: a publish that fails logs and
  returns false, it never throws into the caller's request path. A caller that
  must know the message landed asks for a reply subject, it does not read a
  core publish result as an acknowledgement.
- `publishWithMsgId` is the JetStream publish and is the opposite: it waits for
  the broker's PubAck and never degrades to core NATS, so its false means the
  broker did not store the message and the caller must retain and retry it. The
  durable outboxes settle a row on exactly that return value — there the publish
  result IS the acknowledgement.
- A durable consumer is created with `js_AddConsumer` and then bound
  (`jsSubOptions.Stream` + `.Consumer`), never left to the subscribe call to
  create. cnats deletes the consumer its own `js_Subscribe` created as soon as
  the subscription is unsubscribed or drained — durables included — which would
  reset the cursor to the stream head on every restart and lose exactly the
  backlog a durable exists to retain. Created-then-bound, the consumer outlives
  the process and resumes from its stored cursor.
- `js_AddConsumer` is create-or-update, so an existing durable takes the
  caller's editable fields on every attach (`maxDeliver`, the ack wait, the
  filter, and a fresh deliver subject once nothing is bound to the old one).
  Its deliver policy is fixed at creation: asking for a different `deliverAll`
  is refused by the broker ("deliver policy can not be updated") and the attach
  fails on every retry, so a feed whose policy changes takes a new durable name,
  which is a new cursor. One durable has one bound subscriber: a second
  process's bind is refused ("consumer is already bound to a subscription")
  until the first has left, and its retry then resumes from the shared cursor.
  Both refusals reach the log with the broker's own reason.
- A durable says how many unacknowledged messages it holds
  (`DurableInput::maxAckPending`, editable, so a change reaches an existing
  consumer on its next attach). `kDefaultMaxAckPending` (256) is for a feed
  keyed by id, where order does not matter: a consumer that applies in order
  queues what it has been given, the queue's wait counts against the 60 s ack
  window, and the bound keeps that window from expiring behind the queue unless
  an apply averages more than about 230 ms. `kOrderedMaxAckPending` (1) is for
  a feed whose order is its meaning — the change feeds: the broker delivers
  nothing behind a message until it is acked or given up on, so a redelivery
  can never land after a newer message. A consumer holding a feed table says
  per feed which one it wants (`change_feed::Feed::maxAckPending`,
  `catalog_feed::Feed::maxAckPending`).
- A nak waits before the redelivery: 1 s, doubling per delivery to 30 s. A
  message that fails fast therefore spends its `maxDeliver` over minutes rather
  than milliseconds. The nak of its last delivery does not wait — the broker
  drops the message then, and a delay would only hold an ordered feed longer —
  and logs an error, because the broker keeps no dead letter. A last delivery
  that ends by an expired ack window is dropped without that log.
- An ordered durable waits out its 60 s ack window only for a delivery lost
  while its subscription survives (a connection drop cnats reconnects through).
  A successor process re-attaches through a fresh deliver subject, and the
  broker redelivers the message its predecessor held at once, ahead of the
  rest.
- Attach a durable only once whatever its handler marshals onto exists. A
  durable with a backlog delivers within a millisecond of the bind, on the
  cnats thread, so a handler that reaches `drogon::app().getIOLoop(0)` must be
  bound from a beginning advice or later — before `run()` that loop is null.
- A delivered durable message owns its subscription until it is settled and
  destroyed, so an ack that lands after `unsubscribe` or `drain` fails on a
  closed connection instead of reaching freed memory. `drain` returns only once
  every connection the bus opened has reported closed (bounded at 5 s), because
  cnats delivers that callback later, on its own thread, with the bus as its
  closure.
- `tests/unit/nats-wrapper-test.cc` pins the durable rules against a live
  broker (`ARGUS_TEST_NATS_URL`): the backlog survives an unsubscribe and a
  drain, a second binder is refused while the first is bound, a changed
  deliver policy is refused, and an ordered durable redelivers a nak'd message
  before the one behind it, after the backoff.
- Handlers must not block: a subscription that needs to do real work hands it
  to `BlockingTask` (argus-runtime) rather than doing it on the cnats thread.

## Tests

`tests/unit/nats-wrapper-test.cc` — publish subjects take plain dotted tokens
while subscribe subjects take `>`-only wildcards, the frozen subject
spellings, the option defaults without config, a handler registered without a
server, and a connect to a closed endpoint that fails instead of crashing. The
live roundtrip and the stream reconcile run only when `ARGUS_TEST_NATS_URL`
names a broker, and say so when it does not.

`[nats] user` and `[nats] password`, when both are set, are passed to the
broker with `natsOptions_SetUserInfo`; credentials never go into the URL,
which services log.
