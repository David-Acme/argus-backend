#pragma once

#include "error-code.hxx"

#include <string_view>

// One refusal, spelled once: the code a client switches on, the status the
// envelope carries and the text a human reads. Every catalog entry is one of
// these (architecture plan section 4.7), and a refusal is thrown, never built:
// the getNNNResponse family is gone and the shared advice is the only place a
// definition becomes a response.
struct ErrorDefinition
{
  ErrorCode code;
  int status;
  std::string_view message;

  // The catalog's code and status with the caller's text: what a downstream
  // device or service said, when it said anything at all.
  [[nodiscard]] constexpr ErrorDefinition
  withMessage(std::string_view detail) const
  {
    return ErrorDefinition{code, status, detail};
  }

  // The code as a client reads it, so nothing outside this package has to know
  // that the vocabulary is an enum.
  [[nodiscard]] constexpr std::string_view wireCode() const
  {
    return toString(code);
  }
};
