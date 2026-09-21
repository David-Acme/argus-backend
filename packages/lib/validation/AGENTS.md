# argus-validation

The DTO validation DSL: the macro vocabulary a controller writes to check a
request body, and the `Validator` those macros build, over the shared
validation exception.

## What this is

A PACKAGE, not a service, and the one lib that is genuinely header-only: the
target is INTERFACE and there is no `.cc` anywhere in it. Ten units outside
`packages/lib` link it — identity, memory, sync and seven services — each of
them validating the DTOs it accepts on the wire.

A refusal is a `ValidationException` carrying a per-field error map, never a
message: the controller does not format the answer, `argus::lib::http` does.

## Layout

- `src/validation/validation_dsl.hxx` — the macro vocabulary:
  `START_VALIDATION`/`END_VALIDATION` around `IS_NOT_EMPTY`, `IS_ALPHA`,
  `IS_EMAIL`, `IS_UUID`, `IS_URL`, `IS_IN`, `MIN_LENGTH`/`MAX_LENGTH`,
  `IS_POSITIVE`, `MIN_INT`/`MAX_INT`, `BETWEEN`, `EQUALS_FIELD`,
  `ARRAY_NOT_EMPTY`/`MIN_ELEMENTS`/`MAX_ELEMENTS`, `IS_VALID_TIMESTAMP`,
  `IS_BOOLEAN`, `MATCHES_REGEX`, `CUSTOM_LAMBDA` — with the `_OPTIONAL`
  variants of the rules that would otherwise reject an absent value.
- `src/validation/validator.hxx` — `Validator<DtoType>`: `add<Rule>(...)` and
  `validateOrThrow`, which collects EVERY failure before throwing rather than
  stopping at the first.
- `src/validation/details/rules.hxx` — private by convention: the rule
  implementations the macros name.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME validation
  HEADER_ONLY ...)` declaration, explicit source lists, never `file(GLOB)`.
  The include root is `src/`, so consumers write
  `<validation/validation_dsl.hxx>`; the header-only shape is what keeps the
  macros compilable in a consumer with no extra link line.
- The DSL validates shape, not existence: it never reads the database, never
  calls a service, and never decides authorization. A rule that needs a row to
  answer belongs to the feature service, which throws `ResponseException`
  where the DSL throws `ValidationException`.
- All failures are reported, never the first one: `validateOrThrow` walks
  every rule and throws once with the whole map, because a client that fixes
  one field per round trip is a client that sees a validation bug as a
  conversation.
- A rule is declared once, in the DSL, and named by a macro. A validation
  written inline in a controller is the duplication the DSL exists to remove,
  and it hides the field's rules from the next reader of the DTO.

## Tests

`tests/unit/validation-dsl-test.cc` — 18 cases over the macro vocabulary: the
required and optional forms, the length and range bounds, `IS_IN`, the
array rules and `CUSTOM_LAMBDA`, plus the exception owning its field errors
independently of the validator that produced them.
