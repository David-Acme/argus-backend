# argus-cert

TLS for the instance: a local CA, the server certificate the HTTPS listener
presents, the fingerprints a client pins, and the pairing code a device
proves once.

## What this is

A PACKAGE, not a service: no listener, no route, no `main`. Seven units link
it — identity, sync, camera, gateway, guard, notification and productivity —
because each one either serves TLS, verifies the CA, or hands a client the
fingerprint to pin.

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
- Paths and lifetimes are config, not constants: `cert.dir` seeds
  `ca.pem`/`server.pem` unless `cert.ca_cert`, `cert.ca_key`,
  `cert.server_cert` and `cert.server_key` name the files, and
  `cert.leaf_ttl_days`, `cert.rotation_threshold_days` and
  `cert.rotate_check_hours` decide when `rotateServerCertificate` reissues.
- Nothing here goes on the wire but PEM and fingerprints: the private keys
  stay on disk, and no service reads them through this package's surface.

## Tests

`tests/unit/cert-san-test.cc` — the SAN list and the rotation, against a live
in-process listener; it is the reason this package configures on its own.
