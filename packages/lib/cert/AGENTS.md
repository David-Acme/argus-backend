# argus-cert

TLS for the instance: a local CA, the server certificate the HTTPS listener
presents, the fingerprints a client pins, and the pairing code a device
proves once.

## What this is

A PACKAGE, not a service: no listener, no route, no `main`. One owner links
it — `argus-identity` (`services/identity:144`) with its
`identity-invitation` (`src/feature/invitation:14`) and
`identity-pairing` (`src/feature/pairing:9`) modules — because it either
serves TLS, verifies the CA, or hands a client the fingerprint to pin. Every
other TLS listener reaches Drogon through the `cert.server_cert` /
`cert.server_key` paths in its own config instead of this package's surface
(argus-sync's `/sync` listener is the example), and five services — camera,
guard, notification, productivity, sync — used to pull the folder by path with
no link line behind it; those dead pulls are gone.

It is also one of the two libs declared as a standalone project
(`project(argus-cert)`): its suite boots an in-process TLS listener and
rotates the certificate underneath it. That listener is Drogon's own, over
the rotated pair, so the suite links this package and the TOML reader and
nothing above tier 1 — it used to link `argus_identity` for an identity
route it no longer stands up, and that dead link (plus the identity, ncnn
and migration closure it dragged into this project) went in Phase 2 step 4.

## Layout

- `src/cert/cert-service.{cc,hxx}` — `CertService`: `init`/`isLoaded`/
  `shutdown`, `caPem`, `instanceId`, `caFingerprint`, `serverFingerprint`,
  `pairingCode`/`verifyPairingCode`, `rotateServerCertificate`, `health`.
  The pairing code is read from `<cert.dir>/pairing.code`, a random 12-hex
  secret `scripts/lib/pki.sh` writes 0600; it is never derived and never
  logged. It used to be the first 8 hex of the CA fingerprint, and the CA
  travels in every TLS handshake, so any LAN peer could compute it.
- Rotation writes the new key and chain through a temporary created with
  the final mode (the key 0600, the chain 0644), fsynced and renamed; the
  key used to land 0644 under the default umask. The server fingerprint is
  read and replaced under a lock, since the rotation thread writes it while
  request threads read it. Only argus-identity rotates, so the deploy
  compose mounts the certs directory writable for identity alone; the other
  services pick the new leaf up on their next restart.
  `instanceSans()` and `buildSanString()` are where the SAN list is decided.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME cert ...)`
  declaration, explicit source lists, never `file(GLOB)`. The include root is
  `src/`, so consumers write `<cert/cert-service.hxx>`.
- The SAN is a contract, not a local detail. `instanceSans()` carries
  `argus.local`, `localhost`, `127.0.0.1`, `::1`, the machine hostname,
  `remote.hostname` and `mdns.name` — the name argus-mdns advertises — so
  renaming the appliance without reissuing the certificate breaks TLS at every
  client that pinned the old one.
- `remote.hostname` is validated before it becomes a SAN entry: an invalid
  value is logged and dropped, because a SAN that is not a hostname is a
  certificate clients refuse.
- `mdns.address` (the LAN address provisioning writes) becomes an IP SAN when
  it is a private, CGNAT or link-local address, so an app that reached the
  service by IP validates the leaf; a public address is never baked in.
- Every rotated leaf is a server leaf and says so: `basicConstraints
  CA:FALSE`, `keyUsage digitalSignature` and `extendedKeyUsage serverAuth`.
- A CA created since 2026-10 carries `nameConstraints` (scripts/lib/pki.sh:
  `.local`, `localhost`, the host name, the mDNS name, `remote.hostname` and
  the private/loopback/link-local IP ranges). Rotation drops, with a warning,
  every SAN the CA's constraints do not permit — the container's own host
  name, a `remote.hostname` set after the CA was made — so the leaf it writes
  always verifies; a name that must be added means a new CA and a re-pair.
  A CA without the extension (every install before 2026-10) keeps the full
  SAN list.
- The pairing code file accepts the base32 codes `pki.sh` now writes (26 to 32
  of `A-Z2-7`) and still reads a legacy 8-12 hex code.
- Paths and lifetimes are config, not constants: `cert.dir` seeds
  `ca.pem`/`server.pem`/`pairing.code` unless `cert.ca_cert`, `cert.ca_key`,
  `cert.server_cert`, `cert.server_key` and `cert.pairing_code` name the
  files. The deploy keeps the CA key and the pairing code out of the shared
  `certs/` directory (`ca/ca.key`, `ca/pairing.code`, a directory only
  argus-identity mounts — argus-deploy/CONTEXT.md, "The instance CA"), and
  `cert.leaf_ttl_days`, `cert.rotation_threshold_days` and
  `cert.rotate_check_hours` decide when `rotateServerCertificate` reissues.
- Nothing here goes on the wire but PEM and fingerprints: the private keys
  stay on disk, and no service reads them through this package's surface.

## Tests

`tests/unit/cert-san-test.cc` — the SAN list and the rotation, against a live
in-process listener; it is the reason this package configures on its own.
It also pins the private LAN address becoming an IP SAN (and a public one not),
the leaf's `serverAuth` usage and `CA:FALSE`, and a name-constrained CA whose
rotation keeps the permitted names and leaves the rest out.
