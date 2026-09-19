# argus-tunnel — CONTEXT

## Why the tunnel exists

F5-4 of the `migracion-microservicios` plan moves the Argus backend to a US
host (Rulings CF-CM). The home network cannot accept inbound connections,
so the home runs `argus-tunnel-client` which holds ONE persistent outbound
connection to `argus-relay` on the US host; the relay multiplexes every
device connection over that single link back to the gateway. The carried
payload is the mobile app's TLS: end-to-end between the app and the gateway,
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
| 1 AUTH | 0 | 32-byte HMAC-SHA256(secret, challenge ‖ "argus-tunnel-auth-v1") | client → relay |
| 2 AUTH_OK | 0 | 32-byte HMAC-SHA256(secret, challenge ‖ "argus-tunnel-relay-auth-v1") | relay → client |
| 3 AUTH_FAIL | 0 | empty | relay → client (then link drop) |
| 4 OPEN | streamId | empty | relay → client |
| 5 DATA | streamId | carried bytes (≤ 64 KiB per frame) | both |
| 6 CLOSE | streamId | 1 byte CloseReason | both |
| 7 PING | 0 | empty | client → relay |
| 8 PONG | 0 | empty | relay → client |
| 9 PUSH | 0 | push-intent JSON (F5-5) | relay → client |
| 10 CHALLENGE | 0 | 32 random bytes, fresh per link | relay → client |

CloseReason: 0 normal, 1 busy (stream cap), 2 idle timeout, 3 back-pressure,
4 error.

Control-plane handshake: the relay generates a fresh 32-byte challenge when
it accepts the home link and sends CHALLENGE; the client answers AUTH with
HMAC(secret, challenge ‖ "argus-tunnel-auth-v1"); on verification the relay
activates the link and demonstrates knowledge of the secret back to the
client with AUTH_OK carrying HMAC(secret, challenge ‖
"argus-tunnel-relay-auth-v1"); only then does the client activate the link
(AUTH_FAIL and a bad AUTH_OK proof both drop it). The handshake must
complete inside the 10 s auth timeout or the link is dropped.

Stream lifecycle: the relay allocates stream ids (`openRemote`, latest-wins
home link). A device TCP connection accepted on the relay opens a stream and
sends OPEN; the client dials a fresh TCP connection to the gateway's remote
listener (`[remote] tunnel_port`, F5-1) per OPEN and registers it
(`openLocal`). DATA frames are retransmitted verbatim in both directions;
frame boundaries never map to carried message boundaries. CLOSE propagates
the reason; a local EOF closes the stream normally.

## Push intents (F5-5)

The relay subscribes to `argus.notification.v1.push_intent` over NATS (behind
`[push] enabled`, default off) and forwards each payload as one PUSH frame to
the home client; both ends keep a bounded in-memory intent queue with
received/dropped counters exposed on `/health` (`pushQueued` / `pushReceived`
/ `pushDropped`, plus `pushForwarded` on the relay). The relay buffers intents
while the home link is down and drains them the moment a link authenticates;
the client queues what it receives. Past `push.queue_capacity` the drop is
counted and logged; empty or oversized intents are also rejected at the
ingress with drop accounting (a PUSH frame caps at 256 KiB while NATS accepts
more, so the bound is enforced before the queue — worst case the capacity
times 256 KiB of relay RAM) — nothing persists, and the queue never becomes
the source of truth: the notification row in the gateway's notification.db is.
This is the deliberate trade-off of Ruling CK as implemented: the NATS leg is
fire-and-forget (plain core-NATS publish, no ack/redelivery) and therefore
at-most-once — an intent published while the relay is away from NATS is
silently lost — and the tunnel leg is best-effort with drop accounting,
because the queue only accelerates delivery — the authoritative fan-out to
the device rides the
existing `/sync` bootstrap (the app re-syncs notifications on connect). The
final device-delivery leg (intent → OS push / APNs / FCM) is out of scope:
the intent is display-only, never a command, and never carries alarm/siren
semantics — the client's queue is a terminal sink in this fase. cnats was
added to the relay's dependencies for the subscription; the NATS handler
posts into the PollLoop thread (`postPushIntent`), so the mux core stays
single-threaded.

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

The home-link frame parser and the AUTH challenge are reset on every link
drop: frames pipelined behind a control-plane rejection in the same read
burst are never dispatched, and each new link runs a fresh challenge →
AUTH → AUTH_OK handshake. The relay replaces an existing home link with a
newer authenticated one (latest wins) and rejects device connections while
no home link is authenticated.

## Threat model

Both ends of the home link prove knowledge of the shared secret (constant-
time compare): the client answers the relay's per-link CHALLENGE, and the
relay answers with its own AUTH_OK proof over the same challenge before the
client activates the link. The challenge makes every mac single-use — a
passive eavesdropper on the plaintext home link captures nothing replayable
on a later link — and the AUTH_OK proof lets the client authenticate the
relay, so a path MITM can neither impersonate the home client nor feed it
control frames. The client additionally enforces the gate client-side:
OPEN/PUSH frames received before AUTH completes are ignored, and the frame
parser dies with the link, so an AUTH_FAIL cannot be followed by pipelined
frames in the same read burst. A relay that never sends a valid AUTH_OK
proof cannot get the client to dial the gateway.

Everything else — the device port — is unauthenticated by design: a rogue
device can open streams and reach the gateway's remote listener. The
defense is the gateway's remote gate (`[remote] tunnel_port`
classification, 403 `REMOTE_NOT_ALLOWED` for forbidden routes such as
`/pairing`). Carried TLS means the relay sees ciphertext only; it cannot
inspect or alter the session.

The control plane itself is the exception to that last statement (F5-5): the
home link is plaintext TCP with HMAC challenge auth — it authenticates but
does not encrypt — so PUSH frames carry notification `title`/`body` in
cleartext. A passive observer on the WAN leg, and the relay itself
(necessarily, since it forwards the frames), can read notification display
content; only the carried device streams remain end-to-end TLS. Ruling CK
explicitly allowed a ref-only intent payload (ids + type, with the app
fetching the row over its own TLS `/sync` session) as the mitigation if the
controller prefers to close this exposure; the verbatim display fields were
chosen for F5-5 and the trade-off is accepted and documented here.

## Config

`[tunnel]` carries the shared secret plus link knobs; `[server]` carries the
per-binary listener keys (relay: `host`/`device_port`/`home_port`; client:
`relay_host`/`relay_port`/`gateway_host`/`gateway_port`; health listeners
default 7103 relay / 7104 client); `[push]` (F5-5, relay-only gate) carries
`enabled` (default false) and `queue_capacity` (default 256), plus
`nats.url` for the subscription. The secret must be identical on both
sides and never committed.

## What was NOT changed

- Zero edits to existing services (root `CMakeLists.txt` /
  `CMakePresets.json` gained the subdirectory + presets only).
- No database, no JWT, no device registry: the relay's device→home mapping
  is in-memory and dies with the process. The push-intent queues are
  in-memory too (F5-5); NATS is subscribe-only on the relay, publisher-side
  policy lives in argus-notification / the gateway.
- The mobile app contracts are untouched: the app keeps talking TLS to the
  gateway host through the tunnel's device port.
