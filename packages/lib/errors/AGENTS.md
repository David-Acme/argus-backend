# argus-errors

The error vocabulary and the mechanism that carries it: the code enum, a
definition that pairs a code with its status and message, and the two typed
exceptions every layer throws (response and validation).

## What this is

A PACKAGE, not a service: no database, no listener, no process, no `main`, and
no HTTP framework. It is a leaf — the foundation everything else may link — so
nothing here may depend on anything but the standard library. The envelope that
formats these errors into `{status, info, errors}` is `argus-http`; this package
only declares them. That separation is the whole reason it exists: the eleven
contracts, the four wire clients and four other libs (`auth`, `http`,
`storage`, `validation`) link this package directly rather than take
`argus-http` and Drogon with it.

## Layout

- `src/errors/error-code.hxx` — `enum class ErrorCode` and `toString`: the
  frozen wire string of every code a client can see.
- `src/errors/error-definition.hxx` — `ErrorDefinition` (code + status +
  message) and `withMessage`, the one way a domain hands the client a runtime
  detail without losing the pairing.
- `src/errors/response-exception.hxx` — `ResponseException` in its three
  shapes (a bare message, a definition, a status plus a list of errors) with
  the `ResponseError`/`ResponseErrors` types it carries.
- `src/errors/error-list.hxx` — `error_list::headed`/`entry`/`forModule`: the
  list shape of a refusal that carries one more fact than its code, a catalog
  entry first and detail entries after it (`MODULE_ID`, `ROLE_HOLDER`), so a
  client reads the fact from an entry instead of parsing a message.
- `src/errors/validation-exception.hxx` — `ValidationException` and the
  per-field `ValidationErrors` map the validation DSL produces.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME errors ...)`
  declaration, explicit source lists, never `file(GLOB)`.
- No Drogon, no HTTP type, no JSON type. A package that needs the envelope
  links `argus-http`; a package that needs the vocabulary links this one.
- A code is declared once, here, and consumed as a type. A string literal for a
  code is the duplication that breaks the wire the first time the two copies
  disagree.
- A definition declares the status with the code: the pairing is the catalog's,
  not the call site's, so `throw ResponseException(SomeErrors::Whatever)`
  cannot disagree with itself.
- Keep `toString` exhaustive: every enumerator has a case, and the test table
  has one row per code. A value with no case throws instead of formatting, which
  turns a new enumerator into a test failure.
