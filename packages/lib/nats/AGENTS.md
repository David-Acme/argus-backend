# argus-nats

The NATS event bus every service publishes and consumes through: one wrapper
over cnats, the subject vocabulary, and the push-intent sink the publishers
install at boot.

## What this is

A PACKAGE, not a service: no route, no `main`, no database. Eleven units
outside `packages/lib` link it — identity, memory, socket, sync and seven
services — because the change stream is how a domain tells the others that a
row moved.

`docs/architecture/wire-nats-subjects.md` is the contract this package
implements: every subject constant here appears there, and a subject that is
not in that document is a subject nobody consumes.

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
- The bus is fire-and-forget by design: a publish that fails logs and returns
  false, it never throws into the caller's request path. A caller that must
  know the message landed asks for a reply subject, it does not read the
  publish result as an acknowledgement.
- Handlers must not block: a subscription that needs to do real work hands it
  to `BlockingTask` (argus-runtime) rather than doing it on the cnats thread.

## Tests

`tests/unit/nats-wrapper-test.cc` — publish subjects take plain dotted tokens
while subscribe subjects take `>`-only wildcards, the frozen subject
spellings, the option defaults without config, a handler registered without a
server, and a connect to a closed endpoint that fails instead of crashing. The
live roundtrip and the stream reconcile run only when `ARGUS_TEST_NATS_URL`
names a broker, and say so when it does not.
