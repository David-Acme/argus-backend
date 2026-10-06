# argus-http — context

## The downloader (`src/http/download/`, `argus::lib::http-download`)

Every service installs its own model components from the app (the selectable
modules plan, `docs/history/plans/modules-and-welcome-plan.md`), inside its
container too, so the download is C++ and not a shell script. It lives here
because this package already owns the HTTP wire and Drogon, and the owner chose
on 2026-10-05 not to add libcurl.

### Shape

`downloadFile(DownloadRequest)` blocks and answers a `DownloadResult`
(`done | cancelled | failed`, a `DownloadFailure` reason with a snake-case
spelling from `downloadFailureToString`, `bytesPresent`, `bytesFetched`, the
last HTTP status and a detail). The request names the pinned file (`url`,
`target`, `expectedSize`, lowercase `expectedSha256`), a `CancellationToken`,
an `onProgress(bytesPresent, bytesTotal)` callback, a `DownloadPolicy` and,
optionally, the `ChunkTransport` to use.

Drogon 1.9.13's client buffers a whole response and has no streaming body
callback, so the file is fetched as successive `Range: bytes=a-b` requests of
`policy.chunkBytes` (8 MiB by default). That bounds memory, gives per-chunk
progress and makes resume natural.

Above the transport, and independent of it:

- **Partial state**: `<target>.part` plus `<target>.part.json`
  (`{url, finalUrl, size, sha256, etag, bytes}`). The part is `flock`ed for the
  whole call, so a second download of the same target answers `busy`. Each
  chunk is appended, `fsync`ed, and only then the sidecar's `bytes` advances
  (written to a temporary and renamed); on resume the part is truncated to the
  sidecar's `bytes`, so bytes a crash left unconfirmed are never trusted. A
  sidecar for another url, size or sha256 restarts from zero.
- **Hashing**: SHA-256 streams over the appended chunks; a resume first hashes
  the confirmed prefix from disk. A mismatch deletes part and sidecar and
  answers `hash_mismatch`; nothing is renamed. On success: `fsync`, rename onto
  the target, `fsync` of the directory, sidecar removed. A target already
  present with the pinned size and hash is accepted without a request; one
  that does not match is replaced by the rename.
- **Responses**: every chunk must be a 206 whose `Content-Range` starts at the
  requested offset, does not pass the requested end, matches the body length
  and names the pinned total (a different total is `size_mismatch`, and the
  partial is dropped). A 200 means the server ignored `Range`: accepted only
  when the whole file fits `policy.singleRequestLimit` (16 MiB) and its body is
  exactly the pinned size (`oversize` when larger), otherwise
  `range_unsupported`. A changed ETag between chunks restarts from zero
  (`source_changed` after `maxAttempts` restarts). 416 is `size_mismatch`.
- **Redirects** are followed here, not by the transport: up to
  `maxRedirects` hops (absolute, scheme-relative, rooted and relative
  `Location`s; a non-http scheme is refused). The url that finally answered is
  cached as `finalUrl` and reused for the next chunks; a 403/404/410 on a url
  other than the original (an expired signed CDN url) re-resolves through the
  original, `maxReResolves` times in a row.
- **Retries**: network errors, timeouts, 408, 429 and 5xx retry with
  exponential backoff (`backoffInitial` doubling to `backoffMax`); the budget
  is `maxAttempts` consecutive failures and resets whenever a chunk lands. TLS
  failures and other 4xx stop at once. A failure with no confirmed bytes
  leaves no part file behind.
- **Connections**: after a transport error or a cancellation,
  `DrogonChunkTransport` drops that origin's client so the next request opens
  a fresh connection. Drogon queues later requests behind a timed-out one on
  the same connection, so a retry on the old client never left the process
  (the silent-server test pins it).
- **Cancellation** is checked before every request, while waiting for a
  response (20 ms slices) and during backoff; a cancelled download keeps its
  partial for the next call.

### Running it

The call blocks for as long as the file takes. Run it on a worker thread the
installing service owns (one install at a time), never on a Drogon event loop
— `downloadFile` refuses with `on_event_loop` when called from a running
loop — and not on a `BlockingTask` lane, whose bounded slots are sized for
short jobs. `DrogonChunkTransport` owns its own `trantor::EventLoopThread`, so
it never depends on, or waits for, the application's loops.

### TLS

The Drogon client validates the chain (trantor, OpenSSL default verify paths:
the images need `ca-certificates`). Trantor 1.5's client skips the hostname
check (its `validatePeerCertificate` checks names only on the server side), so
`DrogonChunkTransport` checks the response's peer certificate itself with
`X509_check_host` / `X509_check_ip_asc` and answers `tls` when it does not
name the host. The bytes are also pinned by SHA-256, so a wrong host could
deny the download but never replace the file.

### The fallback path: a libcurl transport

Everything above is written against `ChunkTransport::fetch(ChunkRequest) ->
ChunkResponse` (url, byte range, timeout and cancellation in; transport error,
status, `Content-Range`, `ETag`, `Location` and the body out). The transport
must not follow redirects or retry; those are the downloader's. If real use
shows a case Drogon's client cannot handle (an exotic CDN, a TLS setup, a
redirect form), add a `CurlChunkTransport` beside `DrogonChunkTransport`:

1. Add `libcurl/<version>` to the root `conanfile.txt`. It is already in the
   graph transitively (8.21.0, via `date`), with `with_ssl=openssl`; pinning it
   directly with the unused protocols turned off rebuilds only libcurl
   (checked on 2026-10-05: no other package id changed). Record it in root
   AGENTS.md rule 15.
2. Implement `fetch` with an easy handle per transport, `CURLOPT_RANGE`, no
   `FOLLOWLOCATION`, `CURLOPT_PROTOCOLS_STR "http,https"`, verify peer and
   host, a progress callback that aborts on cancellation, and a write
   callback that refuses more than the requested range.
3. Choose it per request through `DownloadRequest::transport`; the resume,
   hashing, redirect and retry rules and their tests do not change. A libcurl
   transport could also stream one request per file, but the chunked contract
   stays so both transports are interchangeable.

### Tests

`tests/unit/download/download-url-test.cc` pins the url split, `Location`
resolution, `Content-Range` parsing and the certificate host check (on
certificates generated in the test). `tests/unit/download/file-download-test.cc`
runs the downloader twice over: against `FakeTransport` (scripted network
cuts, timeouts, 503/429/401, an expiring CDN url, an ETag change, a lying
`Content-Range`, a TLS failure, a redirect loop, a held lock, a stale or
foreign sidecar, unconfirmed bytes past the sidecar, a verified or corrupt
existing target, a call from an event loop) and against `RangeServer`, a
loopback HTTP/1.1 server in the test with `Range` support, a mid-body
connection drop, a `Range`-ignoring mode, redirects and a silent route, through
the real `DrogonChunkTransport`. They run in every service build that adds
this package.
