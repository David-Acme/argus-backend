# argus_contracts_notification

The notification boundary's vocabulary: the two delivery states a push or a
socket frame settles into.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes
`<notification/notification-delivery-status.hxx>` and links
`argus::contracts::notification`. `packages/sync` is the only consumer: the
notification repository and the delivery service live there, and they are what
writes the column and reads it back.

## Layout

- `src/notification/notification-delivery-status.hxx` —
  `NotificationDeliveryStatus` (`Pending = 0`, `Sent`) with
  `notificationDeliveryStatusToString`/`FromString`. 3 files include it.

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
- Rule 25: the folder IS the module. One `argus_contracts(NAME notification
  ...)` with an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/notification-contract-vocabulary-test.cc` — the two spellings'
  round-trip, the fallback for anything the constraint would reject, and the
  unknown-ordinal path.
