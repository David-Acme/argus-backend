# Device identity wire contract

The device identity contract has two modes selected by the backend config key
`device.identity_mode` (gateway and legacy share the gate in
`backend/src/filter/device/device-filter.cc`):

- `ip` (default): the device hash is HMAC-SHA256 over `userAgent|sourceIp`,
  keyed by `device.fingerprint_secret` (fallback `jwt.secret`). No client
  input participates. This is the legacy behavior and stays byte-identical.
- `credential`: the device hash is HMAC-SHA256 over `userAgent|sha256(secret)`,
  keyed the same way — the source IP never participates.

## `X-Argus-Device-Credential` header

The client-presented credential header. Only `DeviceFilter` reads it; no other
consumer, proxy hop or controller may forward or log it.

- Value: 64 lowercase hex chars — the plaintext of a 32-byte random secret.
- Presentation: every authenticated request of a device enrolled in credential
  mode carries the header next to `Authorization`.
- Server side: only the SHA-256 of the secret is stored (`device_credential`
  table, `secret_hash`); the plaintext is returned exactly once at issuance and
  never logged or persisted.
- Degrade: a missing, unknown, inactive or oversized (> 128 chars) credential
  produces an empty device hash, which fails jwt-filter's session device match
  with the standard `401` `Device mismatch` envelope — never a distinct error.

## Issuance

The plaintext secret is minted at register, at login and at desktop-challenge
approval (credential mode only), always alongside the session it binds:

| Surface | Delivery |
|---|---|
| `POST /auth/register`, `POST /auth/login` | `device_secret` key of the login response (absent in ip mode) |
| `POST /auth/device-login/{challengeId}` approval (mobile) | issued at approval; the desktop receives it as `device_secret` of the first `GET /auth/device-login/{challengeId}` poll that returns `approved` |

A mode flip invalidates every existing session once: old hashes no longer
match, clients re-login and receive a credential. That single controlled
re-login is the accepted cost of switching (blueprint ruling).

The frozen envelope metadata `x_argus_device`
(`proto/argus/common/v1/base.proto` `RequestContext`) is unchanged: it still
carries the resolved device hash the gateway forwards to the legacy backend,
whatever mode produced it.
