# argus_contracts_identity

The identity boundary's refusals: the forty-five answers the enrolment,
pairing, login, portrait and voiceprint flows give.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes `<identity/identity-errors.hxx>` and links
`argus::contracts::identity`. `services/identity` is the only consumer, and
the only owner: the catalog is the identity boundary's own refusal list, and
its forty-five entries are thrown from the enrolment, invitation, pairing,
portrait and voiceprint handlers inside that service.

## Layout

- `src/identity/identity-errors.hxx` — the forty-five definitions in
  `IdentityErrors`, in the header's own order: the face and enrolment answers,
  the pairing and invitation answers, the challenge and refresh-token answers,
  the portrait answers, the five the controllers refuse before any service
  sees the request, the answer a change that could not be recorded gives, and
  the fifteen voiceprint answers (engine unavailable, not enrolled, stale,
  already enrolled, voice already linked, the six sample refusals, consent,
  the challenge, the caller and the owner-assisted face check).
  11 files include it.

## Rules

- A definition declares its status with its code, so a call site cannot
  disagree with itself. Two entries answer 500 while their code says
  `SERVICE_UNAVAILABLE` (`LoginChallengeGenerationFailed`,
  `DeviceCredentialIssuanceFailed`), against 503 for every other
  `SERVICE_UNAVAILABLE` in the tree. Both are the pair the auth catalog also
  declares, which is where today's only two call sites throw them
  (`services/auth/src/feature/auth/services/auth-feature-service.cc:253` and
  `:546`); no source of this service throws either. The catalog suite names
  them so a third cannot arrive unnoticed, and the pair is flagged rather than
  fixed.
- The wire schema is `argus/identity/v1/identity.proto` under
  `packages/contracts/proto/`, compiled by `packages/clients/identity`. This
  package is the C++ half: the refusal list the Drogon handlers throw.
- Rule 25: the folder IS the module. One `argus_contracts(NAME identity ...)`
  with an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/identity-contract-catalog-test.cc` — the forty-five refusals as a
  pinned table, each entry's wire legality, that no two say the same thing,
  and the two entries whose status contradicts their code.
