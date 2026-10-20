#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

inline constexpr size_t kBeaconTokenBytes = 16;
inline constexpr size_t kBeaconSeedBytes = 32;

using BeaconToken = std::array<uint8_t, kBeaconTokenBytes>;

struct BeaconSeed
{
  int64_t userId{0};
  std::vector<uint8_t> seed;
};

struct BeaconTableInput
{
  std::span<const BeaconSeed> seeds;
  int64_t nowSeconds{0};
  int64_t rotateSeconds{900};
};

[[nodiscard]] std::string mintBeaconSeed();

[[nodiscard]] std::optional<std::vector<uint8_t>> beaconSeedBytes(std::string_view base64);

struct BeaconClock
{
  int64_t nowSeconds{0};
  int64_t rotateSeconds{900};
};

[[nodiscard]] int64_t beaconCounter(const BeaconClock& clock);

[[nodiscard]] BeaconToken beaconToken(std::span<const uint8_t> seed, uint64_t counter);

[[nodiscard]] std::string beaconUuid(const BeaconToken& token);

[[nodiscard]] std::optional<std::string> normalizedUuid(std::string_view text);

[[nodiscard]] std::unordered_map<std::string, int64_t> beaconTable(const BeaconTableInput& input);
