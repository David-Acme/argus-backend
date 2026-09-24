# argus-http

The HTTP substrate: the `{status, info, errors}` envelope, the one advice that
turns a refusal into it, the CORS headers, the `/health` controller every
service serves and the listener resolution every service boots from.

## What this is

A PACKAGE, not a service: no database, no domain logic, no `main`, no routes of
its own beyond `/health`. It is the framework boundary of the monorepo — the
only package allowed to know that the wire is Drogon. A service links
`argus-http` and registers the advice in one line; a domain contract never links
it, because contracts declare errors and this package formats them.

## Layout

- `src/http/api-response.hxx` — `ApiResponse`: `ok`, `created`, `noContent`,
  `error` (a raw `ErrorInput`, a `ResponseException`, or an `ErrorDefinition`
  straight from the catalog) and `validationError`.
- `src/http/error-handler.hxx` — `ErrorHandler`: the one advice every service
  registers (`handleException`) plus `unmatchedRoute`, the framework's 404/405.
- `src/http/cors.hxx` — `Cors`: `apply` on a response, `handleOptions` for the
  preflight.
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
  The only responses built by hand here are the three the framework asks for
  directly — an unmatched route's 404, its 405 and the OPTIONS answer — because
  there is no exception to carry them (architecture plan section 4.7).
- Codes come from `argus-errors`; this package declares only the ones no domain
  owns (`http-errors.hxx`). Never a magic status code in a call site.
- The announced set is derived, never listed: a service calls
  `routeAnnouncements({.port = listener.port, .tls = listener.tls})` and a new
  route makes itself discoverable by being registered. A hand-written list of
  paths would drift from the routes without anything failing.
- Keep the envelope's four shapes and the error form (`errors` as an object for
  one failure, an array for many) as they are: clients read them.
