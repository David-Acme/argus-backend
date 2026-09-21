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
  `storage.mode` and the `storage.s3.*` keys — endpoint, bucket, access_key,
  secret_key, region.
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

## Tests

`tests/unit/storage-vocabulary-test.cc` — `StoredFileCategory` round-trips its
strings, and an unknown value falls back to the documented default. The upload
and presign paths need a live endpoint, so they are exercised by the suites of
the domains that own the objects.
