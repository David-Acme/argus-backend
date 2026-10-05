#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pin_hash
{
inline constexpr int kIterations = 600000;
inline constexpr std::string_view kScheme = "pbkdf2-sha256";

struct HashInput
{
  std::string_view pin;
  int iterations{kIterations};
};

[[nodiscard]] std::string make(const HashInput& input);

struct VerifyInput
{
  std::string_view pin;
  std::string_view stored;
};

[[nodiscard]] bool verify(const VerifyInput& input);

[[nodiscard]] bool wellFormedPin(std::string_view pin);

[[nodiscard]] bool trivialPin(std::string_view pin);
}
