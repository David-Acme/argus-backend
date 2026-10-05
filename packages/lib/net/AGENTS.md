# argus-net

The plain-TCP transport the fleet's hand-rolled HTTP legs share: one owned
socket, one bounded connect, one send loop and one receive step.

## What this is

A PACKAGE, not a service: no listener, no config, no log line, no `main`.
Three units link it — the `stt`, `tts` and `llm` clients
(`packages/clients/{stt,tts,llm}`), whose transitional HTTP legs speak to a
loopback or LAN `host:port` without Drogon. Each of them used to carry its own
copy of `SocketGuard`, `connectLoopback`, `parseUrl`, `sendAll` and the
receive loop (audit #111); the copies had drifted (only the `tts` one retried
`EINTR`, honoured a stop token and suppressed `SIGPIPE`), and this package is
the union of their best behaviour. Clients are tier 3 and may not depend on
each other, so the shared piece is a tier-1 lib.

## Layout

- `src/net/loopback-socket.{cc,hxx}` — namespace `argus::net`:
  - `Socket`: the RAII descriptor (move-only, closes once, `shutdown()`).
  - `parseEndpoint(url)`: strips the scheme and the path, splits the port at
    the last `:` (`std::stoi`, so a non-numeric port throws), 80 when absent.
  - `connectLoopback({host, port, timeout, cancellation})`: an IPv4 literal
    only, `SOCK_CLOEXEC | SOCK_NONBLOCK`, a connect bounded by `timeout` that
    retries `EINTR` and polls in 20 ms slices while a stop is possible, a stop
    callback that shuts the socket down, then the socket back in blocking
    mode with `SO_RCVTIMEO`/`SO_SNDTIMEO` = `timeout` and `TCP_NODELAY`.
    Answers a `Connection` (`Socket`, `NetStatus`, the failing `errno`).
  - `sendAll`, `receiveSome`, `readUntilClosed`: `MSG_NOSIGNAL` sends, `EINTR`
    retried, a stop token checked before and after every syscall; they answer
    `NetStatus::{Ok, Closed, Failed, Cancelled}` and never throw.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME net ...)`, explicit
  sources, include root `src/` (`<net/loopback-socket.hxx>`).
- Nothing here throws a domain error or names a service: each client maps a
  `NetStatus` onto its own refusal (`argus-tts unreachable at ...`,
  `TtsErrors::Cancelled`, `argus-llm stream cancelled`), so the wire messages
  stayed byte for byte what they were. A caller that wants a stop to wake a
  blocked receive installs its own `std::stop_callback` over
  `Socket::shutdown()` for the whole exchange, as the clients do.
- HTTP framing (request head, status line, chunked bodies) is not here: it
  still differs per client and stays in each `*-remote.cc`.

## Tests

`tests/unit/loopback-socket-test.cc` — the endpoint parse, a round trip over a
real loopback listener (blocking, close-on-exec, 200 KB answer read to the
peer's close), a refused port reported as `ECONNREFUSED` and a host that is
not an address as `EINVAL`, a stop requested up front cancelling connect, send
and receive, a stop during a blocked receive waking it as `Cancelled`, a silent
peer ending the read at the connection's timeout, and the descriptor closed
exactly once across a move. It runs in every service build that pulls a
consuming client in.
