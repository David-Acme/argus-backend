#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace outbox
{

using MsgIdEntropy = std::array<unsigned char, 16>;

struct TransitionKey
{
  std::string_view prefix;
  std::string_view table;
  int64_t recordId{0};
  std::string_view discriminator;
};

[[nodiscard]] std::string transitionId(const TransitionKey& key);

[[nodiscard]] std::string uniqueId(std::string_view prefix,
                                   const MsgIdEntropy& entropy);

[[nodiscard]] std::optional<MsgIdEntropy> drawEntropy();

[[nodiscard]] std::string legacyId(std::string_view prefix, int64_t rowId);

[[nodiscard]] std::string fingerprint(std::string_view payload);

}
