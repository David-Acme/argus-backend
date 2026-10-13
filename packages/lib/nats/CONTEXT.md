# argus-nats: why the bus is shaped this way

`AGENTS.md` beside this file says what the package holds and the rules for
changing it. This file records the decisions behind them.

## One user per service, inside the one account (S3)

Every service used to reach the broker as one shared user with no
permissions, so any service could publish or read any subject. The S3 ruling
of 2026-10-12 (`docs/history/plans/realtime-and-backlog-plan.md`, section 5)
gives each service its own NATS user with publish and subscribe allow-lists,
inside the existing account. Separate accounts were rejected: JetStream
streams belong to an account, and moving the streams to new accounts would
touch the stored state that the durables resume from.

## The permission table is the one source

`src/nats/nats-permissions.cc` holds one row per service. A row says what the
service does, not which broker subjects that needs: the subjects it publishes
with a core publish, the subjects it stores with a JetStream publish, its core
subscriptions, the streams it ensures, and its durables.
`grantsFor` turns that into broker subjects with the same rules for every row,
so a reviewer reads intent and the expansion is tested once.

The expansion follows what cnats 3.13 sends for each call of `NatsBus`:

| Use in a row | Broker subjects granted (publish) |
|---|---|
| a subject published or stored | the subject itself |
| stream ensured (`NatsBus::ensureStream`, which creates the stream or reconciles an existing one's retention) | `$JS.API.STREAM.CREATE.<stream>`, `$JS.API.STREAM.INFO.<stream>`, `$JS.API.STREAM.UPDATE.<stream>` |
| push durable (`NatsBus::subscribeDurable`: `js_AddConsumer` with `Durable` set, then a bound `js_Subscribe`) | `$JS.API.CONSUMER.DURABLE.CREATE.<stream>.<name>`, `$JS.API.CONSUMER.INFO.<stream>.<name>`, `$JS.ACK.<stream>.<name>.>` (ack, nak, term and in-progress all answer the delivery's reply subject) |
| a durable that is recreated on a policy change (the module feeds) | the above plus `$JS.API.CONSUMER.DELETE.<stream>.<name>` |

A service that stores, ensures a stream or reads a durable also subscribes to
its own inbox: JetStream API answers and publish acknowledgements arrive on
it, and so do push deliveries. The inbox is `_INBOX.<service>`, never the
shared `_INBOX.>`, because a service allowed `_INBOX.>` reads every other
service's deliveries and replies.

## Why the rendering is checked in

The broker reads a config file, and provisioning is a shell script on a host
that has no build. So `render()` writes the grants as one config variable per
service, `ARGUS_NATS_<SERVICE>`, and the result is committed as
`argus-deploy/nats-permissions.conf`. The test fails when the file and the
table disagree, so the file never says more than the table.

nats-server reads an empty `allow` list as no restriction at all, so a
direction with nothing to grant is rendered as `deny: [">"]`; the relay,
which only subscribes, is the case today.

## Limits of subject permissions

The broker checks the subject of a request, not its body. A service allowed
`$JS.API.STREAM.UPDATE.<stream>` can change that stream's subjects, and the
`DURABLE.CREATE` form carries a durable's filter in the body, so a service can
re-create its own named durable with another filter inside a stream it may
read. Neither reaches another service's stream or durable name, which is what
the table bounds.

## The guard advisory grant

The guard row grants the max-deliveries advisory on its `argus-guard` durable
(`$JS.EVENT.ADVISORY.CONSUMER.MAX_DELIVERIES.ARGUS_CAMERA.argus-guard`), which
guard subscribes to. `NatsBus::subscribe` refuses a token with `$` today, so
that subscription does not attach until the subject check admits it; the grant
is in place for when it does.
