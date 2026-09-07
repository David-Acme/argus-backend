# argus-tunnel — CONTEXT

## Why the tunnel exists

F5-4 of the `migracion-microservicios` plan moves the Argus backend to a US
host (Rulings CF-CM). The home network cannot accept inbound connections,
so the home runs `argus-tunnel-client` which holds ONE persistent outbound
connection to `argus-relay` on the US host; the relay multiplexes every
device connection over that single link back to the gateway. The carried
payload is the app móvil's TLS: end-to-end between the app and the gateway,
unterminated by the tunnel (Ruling CF — byte transparency, not gRPC).

## Wire protocol

12-byte header, little-endian, then the payload:

```
offset  size  field
0       2     magic 0xA755 ("AU")
2       1     version (1)
3       1     type
4       4     streamId
8       4     payloadLen (<= 256 KiB)
12      ...   payload
```

| type | id | payload | direction |
|------|----|---------|-----------|
| 1 AUTH | 0 | 32-byte HMAC-SHA256(secret, "argus-tunnel-auth-v1") | client → relay |
| 2 AUTH_OK | 0 | empty | relay → client |
| 3 AUTH_FAIL | 0 | empty | relay → client (then link drop) |
| 4 OPEN | streamId | empty | relay → client |
| 5 DATA | streamId | carried bytes (≤ 64 KiB per frame) | both |
| 6 CLOSE | streamId | 1 byte CloseReason | both |
| 7 PING | 0 | empty | client → relay |
| 8 PONG | 0 | empty | relay → client |
| 9 PUSH | — | reserved F5-5 hook | both |

CloseReason: 0 normal, 1 busy (stream cap), 2 idle timeout, 3 back-pressure,
4 error.

Stream lifecycle: the relay allocates stream ids (`openRemote`, latest-wins
home link). A device TCP connection accepted on the relay opens a stream and
sends OPEN; the client dials a fresh TCP connection to the gateway's remote
listener (`[remote] tunnel_port`, F5-1) per OPEN and registers it
(`openLocal`). DATA frames are retransmitted verbatim in both directions;
frame boundaries never map to carried message boundaries. CLOSE propagates
the reason; a local EOF closes the stream normally. Unknown PUSH frames are
logged and ignored (the F5-5 hook).

## Concurrency model

One PollLoop thread per binary owns every tunnel socket (epoll, level
triggered, level-triggered reads with explicit EPOLL-interest pausing);
Drogon runs its own small thread pool for `/health` only. No locks inside
the tunnel core — single-threaded by construction. Why a hand-rolled engine
instead of trantor: trantor 1.5.x (bundled with drogon/1.9.13) exposes no
public read-pause on TcpConnection, which the back-pressure valves need.

## Back-pressure (bounded buffers)

- Frame parser caps buffered input at `kMaxPayload` and hard-fails on
  desync (link drop).
- Per-stream pending (`pendingToHome` / `pendingToLocal`) caps at
  `stream_pending` 256 KiB: the producer's socket read is PAUSED first
  (true end-to-end TCP back-pressure, no byte loss); a stream is killed
  past 2x the cap (safety net for a stalled far end).
- Global pending valves: local reads pause at `global_pending` 8 MiB; the
  home link read pauses at `link_high_water` 256 KiB queued far-end bytes.
- Peer send buffers: soft limit 256 KiB (congestion notification), hard cap
  4 MiB (peer dropped).
- Resume thresholds are half the corresponding cap.
- Pausing the home link only stops epoll reads; frames already fed into the
  parser from the same read burst keep dispatching. `pumpHomeFrames()` gates
  dispatch on the pause flag and is re-run on resume, so one burst can
  overshoot a cap by at most one read chunk (64 KiB) — never past the 2x
  kill threshold. Without the pump the "safety net" killed healthy streams
  mid-transfer, which would corrupt the carried TLS session.
- `socket_snd_buf` (Limits, default 0 = kernel-managed) bounds SO_SNDBUF on
  every tunnel socket. With kernel autotuning, loopback absorbs ~2.5 MB per
  hop before the software valves ever see congestion, and a fully closed TCP
  window then drips at zero-window-probe pace; a bounded sndbuf keeps
  back-pressure in the software queues where it is observable and fair.

## Reconnect semantics (NatsBus-style)

The client reconnects with a fixed `reconnect_wait_ms` (default 2000, max
`max_reconnects` attempts, then gives up until restart). Streams do NOT
survive a reconnect: dropping the home link tears down every multiplexed
stream on both sides (device sockets are closed, the gateway dial is
dropped) and the app retries at the TLS layer. Why: the relay's stream
registry is ephemeral in-memory state; re-attaching old stream ids after a
registry loss would desynchronize id allocation between the two sides, and
the carried TLS handshake is idempotent from the app's point of view.

The relay replaces an existing home link with a newer authenticated one
(latest wins) and rejects device connections while no home link is
authenticated.

## Threat model

The relay authenticates the home link with the shared secret (constant-time
compare); everything else — the device port — is unauthenticated by design:
a rogue device can open streams and reach the gateway's remote listener.
The defense is the gateway's remote gate (`[remote] tunnel_port`
classification, 403 `REMOTE_NOT_ALLOWED` for forbidden routes such as
`/pairing`). Carried TLS means the relay sees ciphertext only; it cannot
inspect or alter the session.

## Config

`[tunnel]` carries the shared secret plus link knobs; `[server]` carries the
per-binary listener keys (relay: `host`/`device_port`/`home_port`; client:
`relay_host`/`relay_port`/`gateway_host`/`gateway_port`; health listeners
default 7103 relay / 7104 client). The secret must be identical on both
sides and never committed.

## What was NOT changed

- Zero edits to existing services (root `CMakeLists.txt` /
  `CMakePresets.json` gained the subdirectory + presets only).
- No database, no NATS, no JWT, no device registry: the relay's
  device→home mapping is in-memory and dies with the process.
- The app móvil contracts are untouched: the app keeps talking TLS to the
  gateway host through the tunnel's device port.
