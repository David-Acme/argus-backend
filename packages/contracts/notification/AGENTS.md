# argus_contracts_notification

The notification boundary's vocabulary: the two delivery states a push or a
socket frame settles into, the sink a delivery is published through, and the
one refusal the boundary answers with.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes
`<notification/notification-delivery-status.hxx>` and links
`argus::contracts::notification`. Its consumers are the notification service
(the repository, which reads the status vocabulary, and the service, its RPC
service and its two NATS sinks, which carry the sink interface), argus-sync's
`fanout` module (the delivery consumer, its inbox and two suites) and this
package's own two suites. The
delivery sink moved here in sub-step 3a-1b from the engine package, which was
the wrong owner: it carries the notification domain's wire and no sync type at
all.

## Layout

- `src/notification/notification-delivery-status.hxx` —
  `NotificationDeliveryStatus` (`Pending = 0`, `Sent`) with
  `notificationDeliveryStatusToString`/`FromString`. 3 files include it.
- `src/notification/notification-delivery-sink.hxx` — `NotificationDeliveryEvent`
  (delivery id, notification and user ids, type, title, body, `data`,
  `createdAt`), the two-virtual `NotificationDeliverySink` a push backend
  implements (`ensureStream`, `publish`) and `notification_delivery::messageId`,
  the JetStream dedup id. 13 files include it.
- `src/notification/notification-errors.hxx` — `NotificationErrors`, the one
  refusal the boundary throws: the answer a notification write gives when the
  change row that must travel with it could not be recorded, so the write is
  refused and nothing commits. It is the only header here that depends on
  `lib/errors`. 3 files include it.

## Rules

- The two spellings are the contract, not the ordinals: the column is TEXT
  with `CHECK (status IN ('pending', 'sent'))` in
  `services/notification/database/schema.sql`, the ordinals are never stored
  and never sent.
- A spelling the constraint would reject reads back as `Pending` rather than
  throwing, so a value the column cannot hold can never be written back as
  sent. `FromString` matches the two spellings exactly: `"Pending"` and
  `"SENT"` are both `Pending`.
- `ToString` an unknown ordinal formats as `"pending"`. That is deliberately
  not the errors vocabulary's policy, whose `toString` throws on a code it
  does not know: this one has to render a row a newer schema version wrote.
- The wire schema is `argus/notification/v1/notification.proto` under
  `packages/contracts/proto/`, compiled by `packages/clients/notification`.
- A refusal is thrown, never built (root rule 6), and this boundary's are
  catalogued in `notification-errors.hxx` so code, status and text cannot
  drift apart.
- Rule 25: the folder IS the module. One `argus_contracts(NAME notification
  ...)` with an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/notification-contract-vocabulary-test.cc` — the two spellings'
  round-trip, the fallback for anything the constraint would reject, and the
  unknown-ordinal path.
- `tests/unit/notification-contract-catalog-test.cc` — the refusal as a
  pinned table, its wire legality, and that no two say the same thing.
