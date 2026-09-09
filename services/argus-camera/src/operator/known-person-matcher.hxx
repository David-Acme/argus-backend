#pragma once

#include <cstdint>
#include <optional>

// Person crop for the identity seam; x/y/w/h is the box in pixels.
struct PersonCrop
{
  const uint8_t* rgb{nullptr};
  int width{0};
  int height{0};
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
