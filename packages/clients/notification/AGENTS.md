# argus_clients_notification

The argus-notification wire seen from the caller's side: a batch of
notifications in, an outcome and its counters back.

## What this is

A module, not a service: one `argus_clients(NAME notification ...)`, a STATIC
library whose include root is `src/`, so a consumer writes
`<notification/notification-client.hxx>` and links
`argus::clients::notification`. 128 lines of source (`notification-client.hxx`
60, `.cc` 68) behind a 30-line CMakeLists. Six link lines in four CMakeLists:
`argus-guard` (PRIVATE, `services/guard/CMakeLists.txt:104`), `argus::guard`
(`services/guard/src/feature/guard/CMakeLists.txt:22`), `sync-transport`
(`services/sync/src/feature/transport/CMakeLists.txt:21`, the `/sync` pull leg)
and three in `services/notification` — `argus-notification`,
`notification-rpc-test` and `notification-no-nats-test`
(`services/notification/CMakeLists.txt:138`, `:214`, `:240`). The first three
are the callers; the last three are the service that owns the contract, which
builds the generated code through this package for its own server side too, so
the package that serves an RPC links the package that calls it.
`argus/notification/v1/notification.proto` is compiled here and in no other
CMakeLists of the tree.

12 files include the header: this suite, nine in `services/guard`
(`guard-service.cc`, `main.cc` and seven unit suites),
`services/sync/src/feature/transport/infra/notification-sync-gateway.hxx` (the
`/sync` pull leg since sub-step 3a-1c) and
`services/notification/tests/unit/notification-rpc-test.cc`.

## Layout

- `src/notification/notification-client.hxx` — the surface:
  `NotificationRpcOutcome`
  (`Success`, `Conflict`, `Rejected`, `Unavailable`), `NotificationCreateResult`
  (`outcome`, `status`, `created`, `duplicate`), `NotificationPullResult`
  (`outcome`, `status`, `response`), `NotificationClientConfig` (`target`,
  `credential`) and `NotificationClient::createNotifications` /
  `pullNotifications`, both taking `argus::client::CallerIdentity`; 12 files
  include it.
- `src/notification/notification-client.cc` — the deadline (5000 ms,
  `kCallTimeoutMs`, one per call), the metadata, and `outcomeForStatus`.
- `tests/unit/notification-client-test.cc` — the suite below, the newest
  includer.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME notification ...)`
  with an explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<notification/notification-client.hxx>`.
- What a consumer sees: two virtuals, each returning an outcome, the raw
  `grpc::Status` and the decoded body — and the outcome is the contract.
  `ALREADY_EXISTS` is `Conflict`, `UNAVAILABLE` and `DEADLINE_EXCEEDED` are
  `Unavailable`, every other code is `Rejected`, and `Success` is `OK` alone.
  A repeated batch is never a `Success`: the receiver's counters arrive only
  under `OK` (a non-OK status carries no body), so on a conflict `created`
  stays 0 and `duplicate` false — the caller retries on `Unavailable` and
  reconciles on `Conflict`, and the two are deliberately not the same value.
  What it must not see: no `StubInterface`, no `grpc::Channel`, no deadline, no
  generated service class, no retry policy. A target and a credential are the
  whole of what a caller supplies besides the request and the identity, and the
  request and response types are protoc's, which is what it passes in.
- The caller identity is the substrate's: `x-argus-user` (the id in digits) and
  `x-argus-role` always, `x-argus-device` only when the caller's
  `identity.device` is engaged, and `x-argus-credential` only when the
  configured credential is non-empty. An empty credential sends no header at
  all rather than an empty one, and the receiver reads absence as refusal.
- The endpoint and the credential are runtime config, not constants:
  `notifications.grpc_target` and `notifications.credential`, read by
  `services/guard/src/main.cc` (`:131`, `:137`),
  `services/sync/src/config/sync-config.cc:41` and
  `services/sync/src/feature/transport/infra/notification-sync-gateway.cc:63`.
  `argus-deploy/config.guard.toml.example:118-120`
  and `config.sync.toml.example:56-58` declare both, each under a
  `[notifications]` section; the receiver's matching
  secrets are `[grpc] caller_guard` / `caller_sync` in
  `config.notification.toml.example:81-82`, the same placeholder strings, and
  `scripts/lib/common.sh:314-317` fills each pair with one 32-byte hex secret.
  With no credential configured — the empty default in
  `services/*/config.toml.example` — the receiver's `authorizeCaller`
  (`packages/lib/grpc/src/grpc/grpc-server-identity.hxx:58`) matches nothing
  and answers UNAUTHENTICATED before reading the request, which this client
  reports as `Rejected`.
- §2.3 gives a client a `details/` for channel, credentials, retry and envelope
  parsing. This package has **no `details/` directory**: the client is 68 lines
  and the channel, the two headers and the mapping live beside the surface in
  `notification-client.cc`. A known, flagged deviation, not a defect to fix
  here.

## Tests

- `tests/unit/notification-client-test.cc` — two cases against a real
  in-process receiver. The first pins what a create carries: the four
  `x-argus-*` values under a configured credential, the counters (`created`
  2), and then that an unconfigured client with a device-less identity sends
  neither `x-argus-credential` nor `x-argus-device`. The second pins the
  mapping by status — `ALREADY_EXISTS` → `Conflict` with `created` 0 and
  `duplicate` false although the receiver set 9 and true, `UNAVAILABLE` →
  `Unavailable`, `UNAUTHENTICATED` → `Rejected`, an unreachable target →
  `Unavailable` — and then that the same client under `OK` returns the rows
  the receiver served, title and `last_created` id included.
