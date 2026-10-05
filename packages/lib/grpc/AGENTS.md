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
- `src/grpc/fleet-caller-gate.{hxx,cc}` — `FleetCallerGate`, the receiving
  side of the three surfaces that used to share one fleet secret (identity
  7040, sync control 7041, auth 7043). It is built from the service's
  `[rpc.callers]` pairs, the callers it expects and the legacy secret;
  `admit(context, allowed)` names the caller whose credential matched and
  answers `Admitted`, `Forbidden` (a known caller outside `allowed`) or
  `Unauthenticated`. The legacy secret is admitted only while an expected
  caller is unpaired, only for methods an unpaired caller may call, and its
  first use calls `onFirstLegacy` once (the service logs the WARN). A
  `CHANGE_ME` placeholder is never a credential. With nothing configured the
  gate is open, which each service allows only on loopback. Compiled into
  `argus_client_grpc_base`. The sending side is `PeerCredential` and
  `addPeerCredential` in `grpc-client-base`: the caller's own credential as
  `x-argus-credential`, or the legacy secret as `x-argus-fleet` only when it
  has none.
- `src/grpc/grpc-server-drain.hxx` — `argus::client::GrpcServerDrain`, the
  stop of an RPC server a service registers with `shutdown_signal`: it owns
  the `grpc::Server`, `requestStop()` starts `Shutdown(deadline)` on its own
  thread, `drained()` answers once it returned, `stop()` joins and destroys
  the server. Compiled into `argus_client_grpc_base`, so every unit that
  links a client or `grpc-health` has it. It replaced the copies sync,
  notification and productivity each carried; it is namespaced so a unit's
  older global copy still links until that unit switches.
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
- `grpc-health` is the one wire module that belongs to no domain. Two units
  implement its service — camera's and voice's `feature/health/`, each
  overriding `grpc::health::v1::Health::CallbackService` — and nothing calls
  it: no first-party code builds a health stub, and every deployed probe is
  `/health` over HTTP (`argus-deploy/docker-compose.yml`). The five `app/rpc`
  modules that link it (auth, camera, identity, productivity, sync) reference
  no health symbol either: it is what hands a module the `grpcpp/` runtime
  with no domain stub, and for camera's and productivity's it is the only
  such edge, while a module that links a client already has one, the way
  notification's `app/rpc` does. So it is the runtime's rather than a
  generated SDK, and `EXCLUDE_FROM_ALL`, because a unit that pulls this
  package in for the runtime alone must not build it.

## Tests

`tests/unit/grpc-server-identity-test.cc` — `constantTimeEquals` and
`callerCredentialsFromPairs`, the two pieces of the receiving side that need
no channel to exercise.
`tests/unit/fleet-caller-gate-test.cc` — the gate's verdicts: a paired caller
named, missing/wrong/other-caller credentials refused, the legacy secret
refused once every caller is paired and narrowed while one is not, the WARN
hook called once, placeholders never authenticating.
`tests/unit/grpc-server-drain-test.cc` — a live loopback server is shut down
once however often the stop is asked, and a drain without a server is
drained as soon as it is asked to stop.
