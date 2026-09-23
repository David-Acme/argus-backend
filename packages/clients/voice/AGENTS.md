# argus_clients_voice

The argus-voice wire seen from the caller's side: one bidi stream per session,
client frames up and server frames down.

## What this is

A module, not a service: one `argus_clients(NAME voice ...)`, a STATIC library
whose include root is `src/`, so a consumer writes `<voice/voice-client.hxx>`
and links `argus::clients::voice`. 370 lines of source (`voice-client.hxx` 59,
`voice-client.cc` 221, `reaction-contracts.hxx` 90) behind a 40-line
CMakeLists. Two link lines in two CMakeLists: `argus-sync`'s `sync-transport`
module (`services/sync/src/feature/transport/CMakeLists.txt:14`) and
`argus::voice-core` (`services/voice/CMakeLists.txt:86`). argus-sync is the
caller — it relays one app WebSocket onto one gRPC stream — and argus-voice is
the receiver, which links the package for the frames it serves.
`argus/voice/v1/voice.proto` is compiled here and in no other CMakeLists of the
tree.

Three files include `voice-client.hxx`: argus-sync's
`services/sync/src/feature/transport/infra/voice-grpc-relay.hxx`, argus-voice's
`voice-session-service.hxx` and this package's own suite. Three include
`reaction-contracts.hxx` and all three are outside the package: the same two
services, plus `reaction-engine.hxx`.

This is the one client package with a `DEPENDS` edge of its own:
`argus::contracts::auth` (`CMakeLists.txt:21-22`), for the
`<auth/user-role.hxx>` include in `voice-client.hxx:8`.

## Layout

- `src/voice/voice-client.hxx` — the surface: `VoiceStreamObserver`
  (`onServerFrame`, `onStreamClosed`), `VoiceStream` (`start`, `stop`, `skip`,
  `sendPcm`, `finish`), `VoiceClient` (`connect`, `waitConnected`) and
  `voiceRoleToString`; 3 files include it.
- `src/voice/voice-client.cc` — the streaming channel, the reactor, the frame
  encoding and the role vocabulary.
- `src/voice/reaction-contracts.hxx` — the reaction vocabulary, header-only:
  `ReactionKind` (`Idle = 0` … `Alarmed = 9`) with its two round-trip helpers,
  `ReactionSignals` and `Reaction`; 3 files include it.
- `tests/unit/voice-client-test.cc` — the suite below.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME voice ...)` with an
  explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<voice/voice-client.hxx>`.
- What a consumer sees: a `shared_ptr<VoiceStream>` and five calls on it — start
  with an identity, PCM in, skip, stop, finish — plus the observer callbacks
  for what came back and for the close. What it must not see: no
  `StubInterface`, no `grpc::Channel`, no target, no credential, no
  `ClientBidiReactor`, no generated service class, no retry policy. The frames
  are protoc's `ClientFrame`/`ServerFrame`, and `VoiceIdentity` is what the
  caller passes.
- The stream is the client's own: `makeStreamingChannel` (keepalive 30000 ms,
  timeout 10000 ms, pings without data allowed —
  `packages/lib/grpc/src/grpc/grpc-client-base.cc:10-12`) carries a
  `ClientBidiReactor` with **one write in flight** and a queue of at most 1024
  frames; a full queue drops the new frame silently rather than blocking the
  caller (`voice-client.cc:11`, `:140-142`). `finish()` is writes-done, not a
  frame, and `sendPcm` puts bytes on the wire verbatim — no framing, no
  resampling, an embedded NUL and all.
- The identity the stream is gated on is the one passed to `connect`, not the
  one passed to `start`: `connect` sets `x-argus-user` and `x-argus-role`
  (`voice-client.cc:113-117`), `start` puts its own identity inside the first
  frame, and the two may differ. No device and no credential are ever sent —
  there is no field for either — and the receiver asks for exactly the two
  headers the client attaches and checks only that both are present, the roles
  having been validated at the /sync edge (`authorized()`,
  `services/voice/src/feature/voice/voice-rpc-service.cc:78-89`).
  That is the opposite of the productivity edge, where the receiver demands
  all three legs.
- `voiceRoleToString` is the whole role vocabulary: `"owner"`, `"resident"`,
  `"guard"`, `"guest"`, mirroring `UserRole`. The enum has a fixed underlying
  type, so a value outside it is legal, and the switch has no `default:`:
  `VOICE_ROLE_GUEST` breaks out of it and the trailing `return "guest"` answers
  both that role and anything the enum does not name — the least-privileged
  answer, by construction rather than by a case of its own.
- The endpoint is runtime config: `voice.target`, read by
  `VoiceGrpcConfig::resolve` at
  `services/sync/src/feature/transport/infra/voice-grpc-relay.cc:55`.
  `argus-deploy/config.sync.toml.example:48-49` declares it
  (`argus-voice:7034`, the `grpc_port` argus-voice listens on), as does
  `services/sync/config.toml.example:48-49` for the native tree; an empty
  target leaves the leg logging "voice leg -> unconfigured (503)".
- §2.3 gives a client `AGENTS.md`, `CMakeLists.txt`,
  `src/<name>/<name>-*-client.{hxx,cc}`, a `details/` and a unit suite. This
  package has **no `details/` directory** and a second public header the shape
  does not name: `reaction-contracts.hxx` is a vocabulary both ends of the wire
  agree on, which is why all three of its includers live outside the package.
  Known, flagged deviations, left as the layout put them.
- `voice-client.hxx:8` includes `<auth/user-role.hxx>` and the package pays for
  `argus::contracts::auth` on every consumer, but nothing the header declares
  comes from it — it is the only file in the package that names that header.
  The edge is carried for a vocabulary the header does not use;
  flagged, not removed here, because a consumer may be reaching it through this
  package.

## Tests

- `tests/unit/voice-client-test.cc` — two cases. The first pins the role
  vocabulary: four roles by name, and `static_cast<VoiceRole>(42)` answering
  `"guest"`. The second opens a real stream against an in-process server and
  pins what crosses it: `x-argus-user` 7 and `x-argus-role` `"resident"` from
  the connect identity with no device and no credential header; four frames in
  order — a start carrying `start()`'s identity (user 9, `VOICE_ROLE_OWNER`,
  not the one `connect()` got), PCM of exactly three bytes including an
  embedded NUL, skip, stop; the server's `done` reaching the observer with
  `session_id` 4242; and `onStreamClosed` reporting an OK status.
