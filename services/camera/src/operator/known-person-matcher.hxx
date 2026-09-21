#pragma once

#include <camera/identity-state.hxx>

#include <cstdint>
#include <optional>
#include <string>

// Person crop for the identity seam; x/y/w/h is the box in pixels.
struct PersonCrop
{
  int64_t cameraId{0};
  int64_t trackId{0};
  int64_t firstSeenMs{0};
  const uint8_t* rgb{nullptr};
  int width{0};
  int height{0};
  float x{0};
  float y{0};
  float w{0};
  float h{0};
};

enum class PersonIdentity
{
  Unknown,
  Known,
};

// Face-observation state reported for one match() call: the contract's wire
// enum, declared once in contracts/camera because guard reads the same field
// out of the object_detected payload.
struct PersonMatch
{
  PersonIdentity identity{PersonIdentity::Unknown};
  IdentityState state{IdentityState::Unobservable};
  int64_t personId{0};
  float confidence{0.0F};
  int identifyAttempts{0};
};

class IKnownPersonMatcher
{
public:
  virtual ~IKnownPersonMatcher() = default;

  // Nullopt only when the matcher is absent; a present matcher always answers.
  virtual std::optional<PersonMatch> match(const PersonCrop& crop) const = 0;
};

class NoKnownPersonMatcher final : public IKnownPersonMatcher
{
public:
  std::optional<PersonMatch> match(const PersonCrop&) const override
  {
    return std::nullopt;
  }
};
