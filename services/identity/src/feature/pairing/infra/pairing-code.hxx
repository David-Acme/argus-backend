#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct PairingProofCheck
{
  std::string_view nonce;
  std::string_view proof;
};

struct PairingServerProofInput
{
  std::string_view nonce;
  std::string_view caFingerprint;
};

struct PairingHmacInput
{
  std::string_view code;
  std::string_view message;
};

namespace pairing_code
{
inline constexpr std::size_t kSecretBytes = 16;
inline constexpr std::size_t kBase32Length = 26;
inline constexpr std::size_t kMaxLength = 32;

[[nodiscard]] std::string normalize(std::string_view raw);
[[nodiscard]] bool isLegacy(std::string_view code);
[[nodiscard]] bool wellFormed(std::string_view code);
[[nodiscard]] std::string encodeBase32(std::span<const unsigned char> bytes);
[[nodiscard]] std::optional<std::string> mint();
[[nodiscard]] bool sameCode(std::string_view expected, std::string_view candidate);
[[nodiscard]] std::string hmacHex(const PairingHmacInput& input);
}

class PairingCodeStore
{
public:
  explicit PairingCodeStore(std::string path);

  [[nodiscard]] static std::string defaultPath();

  [[nodiscard]] std::optional<std::string> current() const;
  [[nodiscard]] bool verifyCode(std::string_view candidate) const;
  [[nodiscard]] bool verifyProof(const PairingProofCheck& check) const;
  [[nodiscard]] std::string serverProof(const PairingServerProofInput& input) const;
  [[nodiscard]] bool rotate() const;

private:
  std::string path_;
};
