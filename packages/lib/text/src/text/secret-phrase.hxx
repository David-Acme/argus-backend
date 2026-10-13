#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secret_phrase
{

inline constexpr std::string_view kScheme = "phrase-pbkdf2-sha256";
inline constexpr int kIterations = 4096;
inline constexpr int kMinIterations = 1024;
inline constexpr int kMaxIterations = 1000000;
inline constexpr std::size_t kSaltBytes = 16;
inline constexpr std::size_t kMinWords = 3;
inline constexpr std::size_t kMaxWords = 12;
inline constexpr std::size_t kMaxScreenedWords = 96;

using Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] std::vector<std::string> words(std::string_view text);

struct Pbkdf2Input
{
  std::string_view password;
  std::string_view salt;
  int iterations{kIterations};
};

[[nodiscard]] Digest pbkdf2Sha256(const Pbkdf2Input& input);

struct PhraseKey
{
  int iterations{kIterations};
  std::string salt;
  Digest digest{};
  std::size_t words{0};
};

struct SealInput
{
  std::string_view phrase;
  std::string_view salt;
  int iterations{kIterations};
};

[[nodiscard]] std::optional<PhraseKey> seal(const SealInput& input);

[[nodiscard]] std::string encode(const PhraseKey& key);

[[nodiscard]] std::optional<PhraseKey> decode(std::string_view stored);

[[nodiscard]] bool spokenIn(const PhraseKey& key, std::string_view utterance);

}
