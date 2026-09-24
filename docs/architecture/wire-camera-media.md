# Camera media WebSocket (`/media`) contract

Dedicated client-facing WebSocket for camera live media. It is separate from
`/sync` so that best-effort fMP4 never sits in the same TCP egress queue as
low-latency voice PCM and sync frames.

```
App ── wss /media ──> argus-camera:7026 ──> StreamHub ──> go2rtc
                       (DeviceFilter + JwtFilter)
```

- Route: `/media` on argus-camera's app-facing TLS listener
  (`services/camera/src/feature/media/camera-media-socket.hxx`,
  `ListenerConfig::resolveServiceTls("camera", 7026)`), filters
  `DeviceFilter` + `JwtFilter`
  (same auth as `/sync`; token is accepted by header, query or cookie via
  `JwtFilter::extractToken`).
- The app dials argus-camera directly, on the `camera` route it resolved over
  mDNS (`_argus-route._tcp`, TXT `path=camera`). Nothing relays the socket:
  the gateway's byte-transparent `/camera-stream` relay died with the gateway
  in Phase 3d step 1c.
- Only `camera:*` text frames are accepted on the socket. Anything
  else is dropped.
- `argus-camera` serves the same protocol on the same socket for every client;
  there is no second, internal spelling of it.

## Text frames (JSON `{type, payload}`)

| type | direction | payload |
|---|---|---|
| `camera:subscribe` | client → camera | `{cameraId, quality?}` (`quality` is `main` by default, `sub` for the substream) |
| `camera:ready` | camera → client | `{subId, mime:"video/mp4"}` |
| `camera:ack` | client → camera | `{subId, bytes}` credit release |
| `camera:unsubscribe` | client → camera | `{subId}` |
| `camera:<name>_error` | either | `{status, error}` error envelope |

Authorisation is enforced by `argus-camera` (`RolePermission::Read` on
`TableName::Camera`); missing camera rows return a 404 error frame.

## Binary frames (server → client)

Every binary frame is one fMP4 fragment with a 12-byte header
(`services/camera/src/shared/services/stream/ws-frame.hxx`):

| offset | size | field |
|---|---|---|
| 0 | 1 | magic `0xA7` |
| 1 | 1 | version (`1`) |
| 2 | 1 | type (`1` init, `2` media, `3` audio) |
| 3 | 1 | flags (bit 0: keyframe) |
| 4 | 2 | `subId` (big-endian) |
| 6 | 2 | reserved |
| 8 | 4 | `seq` (big-endian) |
| 12 | rest | fMP4 payload (init segment or media fragment) |

Client → server binary frames are ignored; acks are text only.

## Flow control

`StreamHub` gives each subscription a byte credit window
(`streaming.hub_window_bytes`, 128 KiB in the deploy stack). The camera sends
fragments while credit remains and stops until the client releases credit with
`camera:ack`. Slow clients therefore stall only their own camera subscription;
sessions on `/sync` are unaffected. Caps (deploy stack values):

- `streaming.hub_max_subs_per_client = 8`
- `streaming.max_viewers_per_camera = 4`
- `streaming.max_total_viewers = 8`

## Lifecycle and reconnect

- The app opens the socket when a camera must be visible and closes it on
  screen blur/background.
- A dropped socket invalidates its `subId`s: subscriptions and credit state
  live on the socket. Reconnect and re-`camera:subscribe`.
- argus-camera releases the subscription and its viewer slot when the
  connection closes (`StreamHub::closeAll`); once an upstream has no
  subscribers left for `streaming.hub_grace_ms` (2 s), its go2rtc pull is
  closed.
- The endpoint is independent of `/sync` reconnect logic; a camera stream
  never delays sync bootstrap or voice.

## Related

- `docs/architecture/sync-engine.md` — `/sync` operations and bootstrap.
- `docs/history/plans/camera-media-ws-plan.md` — migration plan and rationale.
- `packages/contracts/sync/src/sync/sync-forwarder.hxx` — the shared
  `SyncForwarder` vocabulary both sockets implement.
- `services/sync/src/feature/transport/controllers/sync-socket.hxx` — the
  engine side of that vocabulary.
- `services/camera/src/feature/media/camera-media-socket.{hxx,cc}` — endpoint.
- `services/camera/src/feature/media/camera-media-service.cc` — protocol.
