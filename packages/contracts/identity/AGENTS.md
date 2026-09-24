# argus_contracts_identity

The identity boundary's refusals: the thirty-one answers the enrolment,
pairing, login and portrait flows give.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes `<identity/identity-errors.hxx>` and links
`argus::contracts::identity`. `services/identity` is the only consumer, and
the only owner: the catalog is the identity boundary's own refusal list, and
its thirty-one entries are thrown from the enrolment, invitation, pairing and
portrait handlers inside that service.

## Layout

- `src/identity/identity-errors.hxx` — the thirty-one definitions in
  `IdentityErrors`, in the header's own order: the face and enrolment answers,
  the pairing and invitation answers, the challenge and refresh-token answers,
  the portrait answers, the five the controllers refuse before any service
  sees the request, and the answer a change that could not be recorded gives.
  10 files include it.

## Rules

- A definition declares its status with its code, so a call site cannot
  disagree with itself. Two entries answer 500 while their code says
  `SERVICE_UNAVAILABLE` (`LoginChallengeGenerationFailed`,
  `DeviceCredentialIssuanceFailed`), because both call sites pass 500
  explicitly; every other `SERVICE_UNAVAILABLE` in the tree answers 503. The
  catalog suite names those two so a third cannot arrive unnoticed, and the
  pair is flagged rather than fixed.
- The wire schema is `argus/identity/v1/identity.proto` under
  `packages/contracts/proto/`, compiled by `packages/clients/identity`. This
  package is the C++ half: the refusal list the Drogon handlers throw.
- Rule 25: the folder IS the module. One `argus_contracts(NAME identity ...)`
  with an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/identity-contract-catalog-test.cc` — the thirty-one refusals as a
  pinned table, each entry's wire legality, that no two say the same thing,
  and the two entries whose status contradicts their code.
