#pragma once

#include <cstdint>
#include <optional>

// Identity stays in the gateway/identity domain until Fase 4, so the operator
// evaluates person crops through this seam only. The default implementation
// never matches: rules 3-8 keep their own severity, and known_person (rule 2)
// dominates only when a real matcher is configured.
struct PersonCrop
{
  const uint8_t* rgb{nullptr};
  int width{0};
  int height{0};
  // Box of the person inside the frame, in pixels.
  float x{0};
  float y{0};
  float w{0};
  float h{0};
};

class IKnownPersonMatcher
{
public:
  virtual ~IKnownPersonMatcher() = default;

  // Returns the matched person id, or nothing when the crop is unknown.
  virtual std::optional<int64_t> match(const PersonCrop& crop) const = 0;
};

class NoKnownPersonMatcher final : public IKnownPersonMatcher
{
public:
  std::optional<int64_t> match(const PersonCrop&) const override
  {
    return std::nullopt;
  }
};
