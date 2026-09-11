# Camera media WebSocket (`/camera-stream`) contract

Dedicated client-facing WebSocket for camera live media. It is separate from
`/sync` so that best-effort fMP4 never sits in the same TCP egress queue as
low-latency voice PCM and sync frames.

```
App ── wss /camera-stream ──> gateway ── wss://argus-camera:7026/sync ──> argus-camera
                                (DeviceFilter + JwtFilter)      (same filters)
```

- Gateway route: `/camera-stream`, filters `DeviceFilter` + `JwtFilter`
  (same auth as `/sync`; token is accepted by header, query or cookie via
  `JwtFilter::extractToken`).
- The gateway is a byte-transparent relay: it opens one upstream session per
  client connection with the client's own JWT, User-Agent and a synthesized
  `X-Forwarded-For` (the peer IP the gateway saw; client-supplied forwarded
  headers are never trusted).
- The upstream leg is `[camera] stream_url` in `config.gateway.toml`
  (`ws://argus-camera:7026/media` in the deploy stack). Empty disables the
  endpoint at boot.
- Only `camera:*` text frames are accepted on the client socket. Anything
  else is dropped (the error envelope below is only used for gateway-side
  relay failures).
- `argus-camera` serves the same protocol on its internal `/media` socket
  (`services/argus-camera/src/controllers/camera-media-socket.cc`); no client
  connects to it directly.

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
(`services/argus-camera/src/shared/services/stream/ws-frame.hxx`):

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
- The gateway closes the upstream leg when the client connection closes; the
  camera service releases the subscription and its viewer slot.
- The endpoint is independent of `/sync` reconnect logic; a camera stream
  never delays sync bootstrap or voice.

## Related

- `docs/architecture/sync-engine.md` — `/sync` operations and bootstrap.
- `docs/history/plans/camera-media-ws-plan.md` — migration plan and rationale.
- `packages/argus-sync/src/feature/socket/sync/` — shared `/sync` socket.
- `services/argus-gateway/src/sync/camera-stream-socket.{hxx,cc}` — endpoint.
- `services/argus-camera/src/controllers/camera-media-service.cc` — protocol.
