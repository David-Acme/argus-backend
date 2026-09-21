#pragma once

#include <cstdint>
#include <string>

// The tri-state result of one face-observation attempt, carried as the
// `identityState` field of every track-bound person object in the camera event
// payload. It is declared here, once, because it crosses a service boundary --
// camera writes the spelling, guard and the gateway read it -- and section 2.4
// rule 6 keeps one declaration for the vocabulary that crosses the wire:
// camera's matcher and guard's policy each carried a private copy, which is
// how a wire enum drifts.
enum class IdentityState : uint8_t
{
  Known = 0,
  Unrecognized,
  Unobservable
};

inline std::string identityStateToString(IdentityState state)
{
  switch (state) {
    case IdentityState::Known:
      return "known";
    case IdentityState::Unrecognized:
      return "unrecognized";
    case IdentityState::Unobservable:
      return "unobservable";
  }
  return "unrecognized";
}

// An absent or unrecognised spelling fails closed to Unrecognized, never to
// Known: a producer that invents a name must not be read as an identified
// person. Guard's gate reads the legacy `identity` field beside this one, so
// the empty string keeps its own meaning there and is not folded in here.
inline IdentityState identityStateFromString(const std::string& s)
{
  if (s == "known")
    return IdentityState::Known;
  if (s == "unobservable")
    return IdentityState::Unobservable;
  return IdentityState::Unrecognized;
}
