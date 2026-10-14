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
  attaches to each call. A target written `tls:<host>:<port>` gets mutual TLS
  with the certificate the process installed; every other target stays
  plaintext.
- `src/grpc/internal-tls.{hxx,cc}` — the internal CA's side of S1 (the files
  `scripts/lib/internal-pki.sh` issues): `internalTlsFilesIn(directory)` names
  a service directory's `cert.pem`, `key.pem` and `ca.pem`; `readInternalTls`
  reads them, answering nothing unless the certificate, the key and the trust
  bundle each hold what they should; `internalServerCredentials` requires and
  verifies the client's certificate against the trust bundle;
  `internalChannelCredentials` presents the service's own certificate and
  verifies the server's, its name included. `installInternalTls` hands the
  process its material exactly once, before its first channel: a second
  install throws, an install after any channel throws, and empty material is
  refused (it throws, and does not count as the install), never dropped
  silently. `makeChannel` and `makeStreamingChannel` go through
  `openChannel`, which records under the same lock that the process opened a
  channel, plain or not, and serves a `tls:` target with the installed
  material: with none installed it throws, so a misconfigured service fails
  at start instead of falling back to plaintext. Only the stable `Ssl*Credentials`
  API is used, so the same code builds against the host's gRPC 1.84 and the
  images' 1.51. Compiled into `argus_client_grpc_base`.
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
  gate is open, which each service allows only on loopback. `admit` takes any
  `grpc::ServerContextBase`, so the synchronous servers of tts, stt, vlm and
  llm use it too (with their `[rpc.callers]` pairs, no expected callers and no
  legacy secret); they refuse to start when `pairedCount()` is 0, so their
  gate is never open. `presentedCredential(metadata)` reads the two headers
  and answers nothing when either arrives more than once, which `admit`
  turns into `Unauthenticated`: a credential presented twice is no
  credential, on every surface. Compiled into
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
hook called once, placeholders never authenticating, and a header presented
twice read as no credential at all.
`tests/unit/grpc-server-drain-test.cc` — a live loopback server is shut down
once however often the stop is asked, and a drain without a server is
drained as soon as it is asked to stop.
`tests/unit/internal-tls-test.cc`, run by `scripts/internal-tls-test.sh`,
which issues a scratch internal CA with `scripts/lib/internal-pki.sh` and a
foreign client certificate (exit 77 without openssl) and runs every test case
in a process of its own, since a process installs once — a loopback
`grpc.health.v1` server with mutual TLS answers camera's certificate through
`makeChannel` and `makeStreamingChannel` (`tls:` with `127.0.0.1` and
`localhost`), the client refuses a server whose certificate names another
service, the server refuses a client certificate the internal CA did not
issue, a client without one and a plaintext client, a `tls:` target with
nothing installed throws, a plain target stays plaintext while a certificate
is installed, and the install refuses a second call, a call after
`makeChannel` or `makeStreamingChannel`, and empty material.

`tests/support/grpc-test-support/mutual-tls-endpoint.hxx` — `MutualTlsEndpoint`,
the one test fixture every client's mutual TLS case uses: a loopback
`startListening` with a plaintext port and a mutual TLS port, serving the given
services with the named service's material. It exposes the plain target, the
`tls:` target and the plaintext dial to the TLS port. Its test,
`tests/unit/mutual-tls-endpoint-test.cc`, runs through the same script: a
`makeChannel` `tls:` channel reaches the TLS port with the internal certificate,
and a plaintext channel to that port is refused while the plain port serves.
