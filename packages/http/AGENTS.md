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
- `src/http/http-errors.hxx` — the definitions this package refuses with, all of
  them answers no handler produced.
- `src/http/health-controller.hxx` — `HealthController` (`/health`) and the
  `HealthStatus` a service fills with its own providers.
- `src/http/listener-config.hxx` — `ListenerConfig`, `GrpcListenerConfig`,
  `listenerJson` and `singleListenerJson`.

## Rules

- Rule 25: the folder IS the module. One `argus_module(NAME http ...)`
  declaration, explicit source lists, never `file(GLOB)`.
- The advice is registered once per service and never subclassed; a service
  that needs a different answer changes the definition, not the advice.
- A refusal is thrown, not built: `throw ResponseException(SomeErrors::X)`.
  The only responses built by hand here are the three the framework asks for
  directly — an unmatched route's 404, its 405 and the OPTIONS answer — because
  there is no exception to carry them (architecture plan section 4.7).
- Codes come from `argus-errors`; this package declares only the ones no domain
  owns (`http-errors.hxx`). Never a magic status code in a call site.
- Keep the envelope's four shapes and the error form (`errors` as an object for
  one failure, an array for many) as they are: clients read them.
