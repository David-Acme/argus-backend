# argus-storage

Private-object storage over S3 with a RustFS backing: signed uploads, the
portrait and attachment objects, and the capability-gated reads that keep them
out of the sync stream.

## What this is

A PACKAGE, not a service: no route, no `main`, no database. Three units link
it — identity, camera and guard — which is the whole set of owners of a
private object: the portrait a person has, the face crop a camera stores, and
the encounter evidence guard seals.

## Layout

- `src/storage/s3-storage-service.{cc,hxx}` — `S3StorageService`:
  `isConfigured`, `put`/`putPortrait`, `get`, `remove`, answering
  `S3StoredObject` (object key, sha256, byte size). It resolves
  `storage.mode` and the `storage.s3.*` keys — endpoint, bucket, region,
  credentials_file — on every operation, and refuses, naming the path, when
  the credentials file is empty, unreadable or malformed.
- `src/storage/stored-file-category.hxx` — `StoredFileCategory`
  (`Portrait`, `Attachment`) with its two string converters: the category is
  what drives retention and access policy.
- `src/storage/storage-errors.hxx` — the refusal vocabulary
  (`InvalidObjectUpload`, `InvalidPortraitUpload`, `InvalidPortraitObject`)
  as `ErrorDefinition`s, so a caller throws a coded refusal rather than a
  message.
- `src/storage/details/s3-signing.hxx` — private by convention: AWS SigV4
  (`SigV4Input`, canonical request, signing key), the piece that must not leak
  into a consumer's include path.
- `src/storage/details/s3-credentials.{cc,hxx}` — private by convention too:
  which keys sign a request. `s3_credentials::fromFile` takes the writer's own
  account from the file `credentials_file` names (the RustFS admin API's
  answer, `{"credentials":{"accessKey","secretKey"}}`, which `rustfs-init`
  writes), and nothing else signs a request. The file is read on each
  operation, so an account `rustfs-init` issues again is used without a
  restart.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME storage ...)`
  declaration, explicit source lists, never `file(GLOB)`. The include root is
  `src/`, so consumers write `<storage/s3-storage-service.hxx>`.
- The signing code is `details/` because the algorithm is this package's
  business: a caller asks the service to put or get an object, it does not
  assemble a canonical request. OpenSSL is linked PRIVATE for the same reason.
- Objects here are PRIVATE by construction: a stored object never rides the
  sync stream, so a read is always gated by the capability the owning domain
  checked before it asked. A path that returns an object key to a client
  without that check is the bug this package's shape exists to prevent.
- A refusal is an `ErrorDefinition`, never a bare string, and it is thrown
  through `argus::lib::errors`: the storage layer does not know the HTTP
  status-to-envelope mapping, argus-http does.
- There is no shared key pair: `access_key` and `secret_key` left the
  templates, the provisioning and this package on 2026-10-16, so the keys a
  writer presents are the ones its own bucket-scoped account holds.

## Tests

`tests/unit/storage-vocabulary-test.cc` — `StoredFileCategory` round-trips its
strings, and an unknown value falls back to the documented default.
`tests/unit/s3-credentials-test.cc` — the credentials file is read with the
fields the admin API adds, every unusable file (missing, a directory, not
JSON, a key missing, empty or not a string) is read as nothing, and a request
sent to a loopback listener is signed with the file's keys, the test
recomputing the SigV4 `Authorization` for them; once the file is gone the read
throws and the service reports itself unconfigured. The upload and presign
paths against a real endpoint are exercised by the suites of the domains that
own the objects.
