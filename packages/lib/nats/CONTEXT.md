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
| push durable (`NatsBus::subscribeDurable`: `js_AddConsumer` with `Name` and `Durable` both the durable's name and its one filter, then a bound `js_Subscribe`) | `$JS.API.CONSUMER.CREATE.<stream>.<name>.<filter>`, `$JS.API.CONSUMER.INFO.<stream>.<name>`, `$JS.ACK.<stream>.<name>.>` (ack, nak, term and in-progress all answer the delivery's reply subject) |
| a durable that is recreated on a policy change (the module feeds) | the above plus `$JS.API.CONSUMER.DELETE.<stream>.<name>` |

A service that stores, ensures a stream or reads a durable also subscribes to
its own inbox: JetStream API answers and publish acknowledgements arrive on
it, and so do push deliveries. The inbox is `_INBOX.<service>`, never the
shared `_INBOX.>`, because a service allowed `_INBOX.>` reads every other
service's deliveries and replies.

## The client: its own key, its own inbox

A service whose `[nats]` names `nkey` and `seed_file` connects as its own
user; one that names neither keeps the shared user and password. Both ways
stay valid until every service has switched, and the shared user is removed
last, so a service that still runs an older image or an older config keeps
working through the change.

- **Keys.** The reviewer ruled nkeys over passwords (2026-10-13): the broker
  holds public keys only and each service holds its own seed. cnats
  (`natsOptions_SetNKeyFromSeed`) reads the seed file at every connect, signs
  the broker's nonce and wipes the seed from memory, so the package has no
  signing code and the seed is never a config value.
- **Inbox.** `natsOptions_SetCustomInboxPrefix` moves the connection's request
  replies (JetStream API answers, publish acknowledgements) under
  `[nats] inbox_prefix`. It does not move the deliver subject of a push
  durable: `NatsBus` creates that subject itself, and `natsInbox_Create` knows
  no connection, so `NatsBus::inboxSubject` puts the same prefix on it. A
  durable's deliver subject is editable while nothing is bound, so a durable
  created under `_INBOX` moves to the service's prefix at its next attach.
- **Errors.** A refused publish or subscription reaches the client only as an
  asynchronous `-ERR`; without an error handler a refused subscription just
  receives nothing and a refused JetStream request times out. `NatsBus` logs
  every asynchronous error with the broker's text, which names the subject.

The table, the provisioning and the client were proved together against
nats-server 2.11.14 (`tests/unit/nats-permissions-live-test.cc`, run through
`scripts/nats-permissions-live-test.sh`): every row's own operations pass with
no violation logged, and each refusal the rows imply is logged by the broker.

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

The broker checks the subject of a request, not its body. A durable is
therefore created through the subject that names its filter (ruled
2026-10-13): cnats 3.13 sends `$JS.API.CONSUMER.CREATE.<stream>.<name>.<filter>`
when the config carries a `Name` and one filter subject (and the older
`$JS.API.CONSUMER.DURABLE.CREATE.<stream>.<durable>`, filter in the body, when
it carries only `Durable`), so `NatsBus` sets both and the table grants exactly
that subject per durable, never `DURABLE.CREATE`. A service can create or
re-create only the durables its row lists, each with its own filter. Its
request carries no `action`, so the broker treats it as create-or-update, and a
durable created through `DURABLE.CREATE` by an older client is taken over in
place. Every durable has one filter: `NatsBus::DurableInput` carries one
subject, and no code outside the bus builds a consumer config.

What remains in a body is a stream's configuration: a service allowed
`$JS.API.STREAM.UPDATE.<stream>` can change the subjects of its own stream.
It reaches no other service's stream or durable name, which is what the table
bounds.

## The guard advisory grant

The guard row grants the max-deliveries advisory on its `argus-guard` durable
(`$JS.EVENT.ADVISORY.CONSUMER.MAX_DELIVERIES.ARGUS_CAMERA.argus-guard`), which
guard subscribes to. `NatsBus::subscribe` admits exactly the
`$JS.EVENT.ADVISORY.` prefix (ruled 2026-10-13) and refuses every other `$`
subject: the JetStream API, acknowledgements, flow control, the system account,
key-value and object stores are reached only through the cnats calls that need
them, never through a bare subscription, and nothing publishes a `$` subject
through the bus.
