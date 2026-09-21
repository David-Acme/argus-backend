# argus-grpc

The gRPC runtime every generated client and every RPC server shares: channel
and credential choice, the `x-argus-*` caller metadata, the vendored-stack ABI
bridge, the receiver-side caller checks and the standard `grpc.health.v1`
stubs.

## What this is

A PACKAGE, not a service. It is the one package that declares no `argus_lib`
of its own: its runtime is two OBJECT targets built by the build helpers
(`argus_grpc_client_base`, `argus_grpc_absl_bridge`), which every generated
client module links, and its health stubs are the `argus_client_module`
`grpc-health`. The deviation from rule 25 is deliberate: the ABI bridge exists
to interpose on the vendored gRPC/abseil boundary, so the same objects have to
be declared once, before the first generated client, instead of being compiled
per client.

## Layout

- `src/grpc/grpc-client-base.hxx` — `CallerIdentity`, `makeChannel`,
  `makeStreamingChannel`, `setDeadline`, `addCallerIdentity`: the one place
  channel credentials are chosen for every SDK client.
- `src/grpc/grpc-client-base.cc` — that choice, plus the metadata the base
  attaches to each call.
- `src/grpc/grpc-server-identity.hxx` — the receiving side: `metadata`,
  `constantTimeEquals`, `CallerCredential`, `callerCredentialsFromPairs`,
  `authorizeCaller`, `callerUserId`.
- `src/grpc/grpc-cq-bridge.hxx` and `grpc-cq-bridge-{entry,exit}.cc` — the ABI
  bridge over the system-versus-Conan abseil inline namespaces; the full why
  is in `packages/contracts/CONTEXT.md`. The stubs themselves
  (`grpc/health/v1/health.proto`) live in `packages/contracts/proto/`.

## Rules

- The include root is `src/` (section 2.3, rule 24): consumers write
  `<grpc/grpc-client-base.hxx>` and `<grpc/grpc-server-identity.hxx>`. The
  base target publishes that root PUBLIC, so a unit that links any
  `argus::clients::<domain>` may include these headers; the bridge's entry
  object gets the same root PRIVATE, because the helper compiles it before any
  client module exists.
- Rule 25 still governs the files themselves: one folder, one module, explicit
  source lists, never `file(GLOB)`. Only the declaration differs — the two
  helper functions stand in for `argus_lib`, and `argus::lib::grpc-health` is
  declared by the same `argus_client_module` as every generated client.
- Caller authority comes from the credential that MATCHED, never from declared
  metadata: `authorizeCaller` returns the service name of the credential whose
  secret compared equal in constant time, and answers nothing when the
  presented value is empty. `callerUserId` refuses unless all three legs
  (`x-argus-user`, `x-argus-role`, `x-argus-device`) are present.
- `grpc-health` is the one wire module that belongs to no domain: every
  service that answers health probes implements
  `grpc::health::v1::Health::CallbackService` over it and nothing calls it, so
  it is the runtime's rather than a generated SDK. It is `EXCLUDE_FROM_ALL`,
  because a unit that pulls this package in for the runtime alone must not
  build it.

## Tests

`tests/unit/grpc-server-identity-test.cc` — `constantTimeEquals` and
`callerCredentialsFromPairs`, the two pieces of the receiving side that need
no channel to exercise.
