# argus-runtime — context

## Who uses bounded admission, and why (audit N2, 2026-10-05)

The audit found the bounded lanes (`maxQueued`, `trySubmit`,
`BlockingAdmission::RejectWhenFull`) with no production caller, so every real
queue was still unbounded. The decision was to keep the API and wire it where
a flood can come from outside the process, not to delete it.

Evidence, read off the tree on 2026-10-05:

- **Heavy lane, already wired.** `services/llm`'s and `services/tts`'s HTTP
  stream controllers hand their job to `blocking_pool::trySubmit` and end the
  stream when the lane is full, behind the `StreamSlots` lease that answers
  `Busy` first. A caller hammering `/llm/chat/stream` or `/tts/stream` meets a
  refusal, not a growing queue.
- **Light lane, wired by this change.** Every authenticated HTTP request to
  every app-facing service passes `JwtFilter` (and `DeviceFilter` when the
  request carries `X-Argus-Device-Credential`) before any handler, and each of
  those is a light-lane job blocking on the auth verdict RPC. That is the one
  per-request job an outside client controls the rate of, so both filters now
  go through `auth_admission::admitted` (`packages/lib/auth/src/auth/admitted-call.hxx`):
  `BlockingTask(fn, Light, RejectWhenFull)`, and a full lane becomes
  `AuthErrors::AuthBusy` — 503 `SERVICE_UNAVAILABLE`, "The server is busy,
  retry shortly" — through the one error advice. A forged token never reaches
  the lane: `verifyAccess` checks the HS256 signature on the event loop first.
  `argus-auth`'s face login and registration, the two unauthenticated
  routes that block on identity per request, take the same path. 503 rather
  than 429 because the cause is the whole server's load, not the
  caller's own rate (that is `TooManyAttempts`, the per-key gate in
  `argus-auth`).
- **Everything else stays on `Queue`, deliberately.** The remaining callers
  are reached only after a filter admitted the request (feature services,
  sync gateways, identity directory reads), or run work the process generated
  itself (NATS consumers bounded by their `maxAckPending`, guard's pipeline,
  the call engine, outbox drains, monitors). Refusing those would drop work
  that has no "try again" path — a durable message would need a nak, a monitor
  tick would be lost — while their sources are already bounded upstream.

What would change the decision: a new route that runs a light-lane job
without `JwtFilter` (an unauthenticated endpoint doing blocking IO per
request) must take `RejectWhenFull` the same way; a gRPC handler maps
`BlockingLaneFull` to `RESOURCE_EXHAUSTED`.
