#include "voiceprint-phrases.hxx"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <openssl/rand.h>
#include <span>

namespace
{

size_t randomBelow(size_t bound)
{
  uint32_t value = 0;
  if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1)
    return 0;
  return static_cast<size_t>(value % bound);
}

}

namespace voiceprint_phrases
{

std::vector<std::string> pick(VoiceLang lang, size_t count)
{
  const std::span<const std::string_view> bank =
      lang == VoiceLang::En ? std::span<const std::string_view>(kEnglish)
                            : std::span<const std::string_view>(kSpanish);
  std::vector<size_t> order(bank.size());
  std::iota(order.begin(), order.end(), size_t{0});
  for (size_t index = order.size(); index > 1; --index)
    std::swap(order[index - 1], order[randomBelow(index)]);

  std::vector<std::string> phrases;
  const size_t taken = std::min(count, bank.size());
  phrases.reserve(taken);
  for (size_t index = 0; index < taken; ++index)
    phrases.emplace_back(bank[order[index]]);
  return phrases;
}

}
