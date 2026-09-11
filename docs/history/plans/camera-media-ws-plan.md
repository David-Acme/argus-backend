# Argus — Dedicated camera media WebSocket plan

**Recipient:** implementation agent
**Project:** Argus backend (+ sibling frontend)
**Path:** `/home/acme/Desktop/argus/backend`
**Date:** 2026-09-11
**Status:** implemented; the "before editing" snapshot below names the pieces
that were replaced.

## 1. Objective

Move camera media out of the shared `/sync` WebSocket into its own
authenticated endpoint on the gateway:

- `/sync` keeps sync operations, gateway emits and voice.
- Camera control (`camera:subscribe/ack/unsubscribe`) and fMP4 frames travel
  over a new `/camera-stream` WebSocket.
- The tunnel is untouched: a second WebSocket is just another tunnel stream
  (`tunnel.max_streams` defaults to 256).
- No compatibility window is needed: the frontend does not implement camera
  streaming today (verified: no `camera:*` client, no player dependency).

The problem being solved is head-of-line blocking: one TCP connection
carrying ~2–6 Mbps of best-effort video in front of low-latency voice PCM and
small sync frames, with FIFO egress and no prioritization.

## 2. Current state (verified, before editing)

Backend:

- Shared WS controller `SyncSocket` (`packages/argus-sync/src/feature/socket/sync/socket/sync-socket.hxx`)
  registers `WS_PATH_ADD("/sync", "DeviceFilter", "JwtFilter")`.
- Gateway (`services/argus-gateway/src/main.cc:202-212`) wires
  `CompositeSyncRelay(LegacySyncRelay(camera.sync_url), VoiceGrpcRelay)`.
- `LegacySyncRelay` (`services/argus-gateway/src/sync/sync-relay.{hxx,cc}`)
  opens `ws://argus-camera:7026/sync` per client with the client's JWT
  (`Authorization: Bearer`), the client User-Agent and a synthesized
  `X-Forwarded-For`, and relays bytes in both directions.
- `SyncService::handleMessage` (`packages/argus-sync/src/feature/socket/sync/services/sync-service.cc`)
  forwards only `camera:*` and `voice:*`; unknown types get a 400 error.
- Camera service (`services/argus-camera/src/main.cc:118-120`) registers the
  same `SyncSocket` with `CameraMediaService` as forwarder.
  `CameraMediaService` (`services/argus-camera/src/controllers/camera-media-service.cc`)
  implements the media protocol: 12-byte header `0xA7` magic
  (`services/argus-camera/src/shared/services/stream/ws-frame.hxx`), credit
  window (`hub_window_bytes = 131072`), per-camera and global viewer caps.
- Gateway native paths (`services/argus-gateway/src/proxy/proxy-config.cc:5-17`)
  list `/sync`; everything else not native is proxied by
  `SimpleReverseProxy` (camera prefixes `/camera`, `/zone`).
- Gateway tests cover `relayAllowedText`, `relayLegIsCamera` and
  `CompositeSyncRelay` (`services/argus-gateway/tests/gateway-test.cc:271,688-726`).
- `SyncSocket::handleNewMessage` logs every text frame at INFO
  (`sync-socket.cc`): noise that scales with `camera:ack` traffic.

Frontend (`/home/acme/Desktop/argus/frontend`):

- Single socket owner: `src/core/services/sync/synchronize.service.ts`
  (field `socket` at `:76`, URL construction `:306-313`, reconnect `:394-407`,
  401 refresh `:317-326`, binary fan-out `:198-201,377-380`).
- `netService.openSocket` already supports N sockets with strict certificate
  pinning (Nitro Android/iOS, Tauri web).
- **No camera stream client exists**: no `camera:subscribe`, no `0xA7` parser,
  no fMP4/player integration (`react-native-video`/`expo-video` absent).
  Camera UI is metadata + HTTP control only, deliberately preview-less
  (`src/shared/components/dashboard/camera-tile.tsx:20-24`).
- Voice consumes every binary frame as PCM s16le
  (`src/core/services/voice/voice.service.ts:287-295`), so camera cannot share
  the socket without a demux.

Tunnel:

- `services/argus-tunnel` multiplexes device TCP connections over one outbound
  link by stream id (`src/core/tunnel-mux.hxx:57`, cap 256). No change needed.

## 3. Target design

```
App ── wss /sync ──────────> gateway  (sync ops + emits + voice:* + PCM)
App ── wss /camera-stream ─> gateway ── relaya ──> ws://argus-camera:7026/media
                                                     (camera:* + fMP4 0xA7)
```

Gateway:

- New `CameraStreamSocket` WS controller at `/camera-stream` with
  `DeviceFilter` + `JwtFilter`, backed by one `CameraStreamRelay`
  (`[camera] stream_url`) shared across client connections (the relay is
  already session-keyed by connection pointer).
- Only `camera:*` text frames are accepted; binary from the camera is relayed
  as-is; client binary is ignored (acks are text).
- `/sync` forwards `voice:*` to `VoiceGrpcRelay` when configured, otherwise it
  has no forwarder.
- `/camera-stream` joins `gatewayNativePaths()` so the reverse proxy never
  captures it.

Camera service: the internal socket moves to `/media`, served by a dedicated
`CameraMediaSocket` controller over `CameraMediaService`; the sync machinery
(`SyncSocket`, rooms, user directory) is no longer linked into this domain.

Protocol: unchanged (`camera:subscribe`, `camera:ready`, `camera:ack`,
`camera:unsubscribe`, fMP4 `0xA7` frames, credit window). The only client
change is the URL it connects to.

## 4. Phases

### Phase 0 — protocol doc (small, blocks frontend)

- Add `docs/architecture/wire-camera-media.md`: endpoint, filters/auth, frame
  header table, credit/ack semantics, reconnect behavior, caps
  (`max_subs_per_client`, `max_viewers_per_camera`, `max_total_viewers`).
- Link it from `docs/architecture/contracts-overview.md`.

### Phase 1 — gateway endpoint (backend core)

- New `services/argus-gateway/src/sync/camera-stream-socket.{hxx,cc}`:
  - `WS_PATH_ADD("/camera-stream", "DeviceFilter", "JwtFilter")`.
  - `handleNewConnection` → `relay->onConnect(req, conn)`.
  - `handleNewMessage`: text only; non-`camera:` frames answer the
    `{type:"<type>_error", status, error}` envelope; `forwardText` runs
    inside `drogon::async_run`.
  - `handleConnectionClosed` → `relay->onClose(conn)`.
- New `services/argus-gateway/src/sync/camera-stream-relay.{hxx,cc}`: the
  byte-transparent relay with the camera-only frame filter built in.
- `main.cc`: build the relay from `CameraStreamConfig::resolve()` and register
  the controller only when `camera.stream_url` is non-empty; `/sync` keeps
  only the voice leg.
- `proxy-config.cc`: add `/camera-stream` to `gatewayNativePaths()`.
- Tests: frame-filter coverage, stream-config resolution and native-path
  coverage for `/camera-stream`.

### Phase 2 — camera media socket

- Add `CameraMediaSocket` (`/media`, DeviceFilter + JwtFilter) over
  `CameraMediaService`; set the `JwtContext` on connect.
- Remove the `SyncSocket`/`IdentityUserDirectory` wiring from argus-camera:
  the domain no longer serves table sync over WS (the gateway pulls tables
  over gRPC).
- Demote `SyncSocket: text` logging from INFO to DEBUG (it is per-message and
  dominated by acks).
- Update gateway `CONTEXT.md` (`/sync` section) and camera `CONTEXT.md`
  (media socket owns `camera:*`; no client-facing change).

### Phase 3 — frontend connection + protocol client (no video yet)

- Extract a reusable `SocketConnection` helper from `synchronizeService`
  (connect timeout, exponential backoff, `netService.refreshAddress()`,
  401 → single `refreshSession()` retry). `synchronizeService` keeps its
  behavior and uses the helper; voice untouched.
- New `src/core/services/camera/camera-stream.service.ts`:
  `subscribe(cameraId, quality)`, `unsubscribe(subId)`, `onReady`, `onFrame`,
  `onError`; parses the 12-byte header; acks every ~64 KiB; opens lazily on
  camera screen focus and closes on blur/background.
- Constants: `CAMERA_WS_PATH = '/camera-stream'` in a new
  `src/shared/constants/camera.constant.ts`.
- Update `frontend/AGENTS.md` and `frontend/CONTEXT.md` (socket map).

### Phase 4 — player (frontend native, largest chunk)

- Android: Media3/ExoPlayer fed by a custom `DataSource` or a loopback HTTP
  bridge over the fMP4 buffer, inside the existing `argus-net` native module.
- iOS: `AVSampleBufferDisplayLayer` (or the same loopback bridge for
  `AVPlayer`).
- Wire `src/app/cameras/[id].tsx` to the stream (loading/live/error states).
- This phase needs an explicit frontend decision on the player mechanism
  before coding; the backend plan does not depend on it.

### Phase 5 — end-to-end verification

- Update `/tmp/opencode/e2e/camera-stream.mjs` to `/camera-stream`.
- Real C225: subscribe main + sub, multiple subs, unsubscribe, credit window;
  verify camera control (PTZ/settings) still HTTP-only.
- Concurrency: `/sync` + `/camera-stream` + voice session at once; stop acking
  on the camera socket and confirm only video stalls while sync/voice stay
  responsive.
- Tunnel path: two streams (sync + camera) over one outbound link.
- Gates: `./scripts/build-all.sh dev --only argus-gateway` (and camera if
  touched), 0 errors / 0 warnings; gateway tests green.

## 5. Files expected to change

Backend:

- `services/argus-gateway/src/main.cc`
- `services/argus-gateway/src/sync/camera-stream-relay.{hxx,cc}` (new)
- `services/argus-gateway/src/sync/camera-stream-socket.{hxx,cc}` (new)
- `services/argus-camera/src/controllers/camera-media-socket.{hxx,cc}` (new)
- `services/argus-gateway/src/proxy/proxy-config.cc`
- `services/argus-gateway/tests/gateway-test.cc`
- `packages/argus-sync/.../sync-socket.cc` (log level)
- `services/argus-gateway/CONTEXT.md`, `services/argus-camera/CONTEXT.md`
- `docs/architecture/wire-camera-media.md` (new) + contract links

Frontend:

- `src/core/services/sync/synchronize.service.ts` (use extracted helper)
- `src/core/services/net/socket-connection.ts` (new)
- `src/core/services/camera/camera-stream.service.ts` (new)
- `src/shared/constants/camera.constant.ts` (new)
- `src/app/cameras/[id].tsx`, `AGENTS.md`, `CONTEXT.md`
- Native player module (Phase 4)

## 6. Risks and mitigations

- **Proxy route capture**: `/camera-stream` starts with `/camera`. The native
  path list is checked before proxying; add the path and a test proving the
  proxy never sees it.
- **Second connection auth**: token refresh and backoff live in
  `synchronizeService`; extract instead of duplicating (root AGENTS rule 23).
- **Device fingerprint**: both sockets go through `DeviceFilter` with the same
  UA/IP; the camera socket must present the same implicit UA as `/sync`
  (native default) or the device hash will differ.
- **Tunnel stream count**: one extra stream per camera viewer; cap is 256 and
  `Busy` closes are handled by the app reconnect path.
- **Backgrounding**: close the camera socket and stop decoding on app
  background; never keep the player alive off-screen.
- **Voice regression**: do not modify `VoiceGrpcRelay`; its behavior on
  `/sync` must remain byte-identical.

## 7. Acceptance criteria

- `/sync` no longer carries `camera:*`; a camera frame sent there returns
  `camera:<name>_error`.
- `/camera-stream` streams real fMP4 from the C225 through the gateway with
  multiple subscriptions, acks and unsubscription; works through the tunnel.
- While a camera streams, sync events and voice PCM keep flowing without
  added latency (same-socket tests before/after).
- 0 errors / 0 warnings on touched projects; gateway tests and live E2E pass.

## 8. Delivery status

Backend (committed, live-verified with the C225): `/camera-stream` endpoint,
`/media` camera socket, voice-only `/sync`, viewer caps and the protocol doc.
Frontend: `modules/argus-camera` (Android Media3 + iOS AVSampleBufferDisplayLayer
with an fMP4 parser), `cameraMediaService`, the live view on the camera screen
and the prebuild patch script; `bunx tsc`, `bun run lint` and the Android
`assembleDebug` build pass. Pending: iOS compile on macOS and an on-device
run-through (the environment here has no iOS toolchain).

## 9. Out of scope

- Recording, storage, WebRTC, camera audio/talk (talk stays HTTP).
- WebTransport/QUIC experimentation.
- Any change to the tunnel wire protocol.
