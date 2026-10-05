# argus-http

The HTTP substrate: the `{status, info, errors}` envelope, the one advice that
turns a refusal into it, the CORS headers, the `/health` controller every
service serves but guard — which registers its own hand-rolled answer
(`services/guard/src/app/main.cc:60-72`) — and the listener resolution every
service boots from.

## What this is

A PACKAGE, not a service: no database, no domain logic, no `main`, no routes of
its own beyond `/health`. It is the framework boundary of the monorepo and the
one package that owns the HTTP wire: the envelope, the advice and the CORS
headers are declared here, and no other package builds a response directly —
`lib/auth`'s filters answer through the envelope. The three that do build one
are services (tts's and llm's streaming controllers, guard's hand-rolled
`/health`) and, in a package, only `clients/vlm`'s test fake; other packages do
name Drogon's types (its ORM client in `lib/sqlite`, its JSON in `lib/nats`),
but none of them shapes an answer. A service links `argus-http` and registers
the advice in one line; a domain contract never links it, because contracts
declare errors and this package formats them.

## Layout

- `src/http/api-response.hxx` — `ApiResponse`: `ok`, `created`, `noContent`,
  `error` (a raw `ErrorInput`, a `ResponseException`, or an `ErrorDefinition`
  straight from the catalog) and `validationError`.
- `src/http/error-handler.hxx` — `ErrorHandler`: the one advice every service
  registers (`handleException`) plus `unmatchedRoute`, the framework's 404/405.
  An exception that is neither a `ResponseException` nor a
  `ValidationException` is logged with its method, path and text, and the
  client receives the generic `InternalError`: its text is SQLite's,
  storage's or the standard library's, which the caller cannot act on and
  should not read. A `Json::LogicError` - jsoncpp refusing `asString()`/`asInt64()` on a
  field of another type, which the DTO factories call - answers a 422 on
  `body` and is logged as a warning: a request with `"isAllDay":"yes"` was
  the client's mistake, not a server error.
- `src/http/certificate-reload.hxx` — `certificate_reload::watch(listener)`:
  every ten minutes it compares the listener's certificate and key mtimes
  and, once a rotated pair loads, matches and is not expired, calls
  `drogon::app().reloadSSLFiles()`. identity rotates the shared leaf 30 days
  before its 90-day expiry; without this a service that was not restarted
  in that window kept serving the old leaf until TLS failed. A half-written
  rotation (the key and the certificate land in two renames) is skipped and
  retried, because trantor throws on the IO loop when a context does not
  load. `usablePair` is the check.
- `src/http/cors.hxx` — `Cors`: `apply` on a response, `handleOptions` for the
  preflight. Since the 2026-10 audit it sends no `Access-Control-Allow-Origin`
  at all: the app is native and needs none, and the old `*` let any web page
  open in the LAN read the unauthenticated routes.
- `src/http/details/http-errors.hxx` — the definitions this package refuses with, all of
  them answers no handler produced.
- `src/http/health-controller.hxx` — `HealthController` (`/health`) and the
  `HealthStatus` a service fills with its own providers.
- `src/http/listener-config.hxx` — `ListenerConfig`, `GrpcListenerConfig`,
  `listenerJson` and `singleListenerJson`. `ListenerConfig::resolveServiceTls`
  is what an app-facing service boots from: it reads the service's own section
  (`host`, `port`, `plain`, `min_protocol`) plus `cert.server_cert` and
  `cert.server_key`, so TLS is on unless `plain = true` says otherwise.
  `ListenerConfig::resolve` is the internal/plain form the AI services use, and
  `GrpcListenerConfig::resolve` keeps reading `server.host` + `server.grpc_port`.
- `src/http/logical-routes.hxx` — `logicalRoutes()`: the leading path segment of
  every route the app has registered, deduplicated and sorted, which is the set
  a service announces. `/health` and non-`/` patterns are skipped.
- `src/http/route-announcements.hxx` — `routeAnnouncements()`: one
  `_argus-route._tcp` `MdnsInstance` per logical route, TXT `path=<segment>` and
  `https="true"` when the listener terminates TLS. It is the join between this
  package and `argus-mdns`, and it lives here because `lib/mdns` is tier 1 and
  may not consume a contract.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME http ...)`
  declaration, explicit source lists, never `file(GLOB)`.
- The advice is registered once per service and never subclassed; a service
  that needs a different answer changes the definition, not the advice.
- A refusal is thrown, not built: `throw ResponseException(SomeErrors::X)`.
  The three the framework asks for directly are the only answers no exception
  carries — an unmatched route's 404 and its 405, which `ErrorHandler` builds
  through the same envelope factory, and the OPTIONS answer, the one response
  constructed directly (`cors.cc`) — because there is no handler of ours to
  throw from (architecture plan section 4.7).
- Codes come from `argus-errors`; this package declares only the ones no domain
  owns (`http-errors.hxx`). Never a magic status code in a call site.
- The announced set is derived, never listed: a service calls
  `routeAnnouncements({.port = listener.port, .tls = listener.tls})` and a new
  route makes itself discoverable by being registered. A hand-written list of
  paths would drift from the routes without anything failing.
- Keep the envelope's four shapes and the error form (`errors` as an object for
  one failure, an array for many) as they are: clients read them.
