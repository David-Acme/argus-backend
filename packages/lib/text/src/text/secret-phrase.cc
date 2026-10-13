#include "secret-phrase.hxx"

#include "sha256.hxx"

#include <utf8proc.h>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <ranges>
#include <span>
#include <utility>

namespace secret_phrase
{
namespace
{
constexpr std::size_t kBlockBytes = 64;
constexpr unsigned kInnerPad = 0x36U;
constexpr unsigned kOuterPad = 0x5cU;
constexpr unsigned kLowNibble = 0x0FU;
constexpr std::size_t kEncodedParts = 5;
constexpr std::string_view kHexDigits = "0123456789abcdef";

constexpr auto kFoldOptions = static_cast<utf8proc_option_t>(
    static_cast<unsigned>(UTF8PROC_STABLE) | static_cast<unsigned>(UTF8PROC_COMPOSE) |
    static_cast<unsigned>(UTF8PROC_CASEFOLD) | static_cast<unsigned>(UTF8PROC_STRIPMARK));

std::string folded(std::string_view text)
{
  if (text.empty())
    return {};
  utf8proc_uint8_t* mapped = nullptr;
  const utf8proc_ssize_t size = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
                                             static_cast<utf8proc_ssize_t>(text.size()),
                                             &mapped,
                                             kFoldOptions);
  if (size < 0 || mapped == nullptr) {
    if (mapped != nullptr)
      utf8proc_free(mapped);
    return {};
  }
  std::string out(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(size));
  utf8proc_free(mapped);
  return out;
}

bool wordCodepoint(utf8proc_int32_t codepoint)
{
  switch (utf8proc_category(codepoint)) {
    case UTF8PROC_CATEGORY_LU:
    case UTF8PROC_CATEGORY_LL:
    case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
    case UTF8PROC_CATEGORY_ND:
    case UTF8PROC_CATEGORY_NL:
    case UTF8PROC_CATEGORY_NO:
      return true;
    default:
      return false;
  }
}

std::string joined(std::span<const std::string> parts)
{
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty())
      out.push_back(' ');
    out += part;
  }
  return out;
}

class Hmac
{
public:
  explicit Hmac(std::string_view key)
  {
    std::array<std::uint8_t, kBlockBytes> block{};
    if (key.size() > kBlockBytes) {
      argus::hash::Sha256 shortened;
      shortened.update(key);
      std::ranges::copy(shortened.digest(), block.begin());
    }
    else {
      std::ranges::transform(key, block.begin(), [](char value) { return static_cast<std::uint8_t>(value); });
    }
    std::array<std::uint8_t, kBlockBytes> inner{};
    std::array<std::uint8_t, kBlockBytes> outer{};
    std::ranges::transform(block, inner.begin(), [](std::uint8_t value) {
      return static_cast<std::uint8_t>(static_cast<unsigned>(value) ^ kInnerPad);
    });
    std::ranges::transform(block, outer.begin(), [](std::uint8_t value) {
      return static_cast<std::uint8_t>(static_cast<unsigned>(value) ^ kOuterPad);
    });
    inner_.update(inner.data(), inner.size());
    outer_.update(outer.data(), outer.size());
  }

  [[nodiscard]] Digest sign(std::span<const std::uint8_t> message) const
  {
    argus::hash::Sha256 inner = inner_;
    inner.update(message.data(), message.size());
    const Digest innerDigest = inner.digest();
    argus::hash::Sha256 outer = outer_;
    outer.update(innerDigest.data(), innerDigest.size());
    return outer.digest();
  }

private:
  argus::hash::Sha256 inner_;
  argus::hash::Sha256 outer_;
};

std::string hexOf(std::span<const std::uint8_t> bytes)
{
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t byte : bytes) {
    const auto value = static_cast<unsigned>(byte);
    out.push_back(kHexDigits[value >> 4U]);
    out.push_back(kHexDigits[value & kLowNibble]);
  }
  return out;
}

std::optional<std::string> bytesOf(std::string_view hex)
{
  if (hex.size() % 2 != 0)
    return std::nullopt;
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(hex.data() + index, hex.data() + index + 2, value, 16);
    if (error != std::errc{} || end != hex.data() + index + 2)
      return std::nullopt;
    out.push_back(static_cast<char>(value));
  }
  return out;
}

std::vector<std::string_view> fieldsOf(std::string_view stored)
{
  std::vector<std::string_view> parts;
  for (const auto part : std::views::split(stored, '$'))
    parts.emplace_back(part.begin(), part.end());
  return parts;
}

template <typename Number>
std::optional<Number> numberOf(std::string_view text)
{
  Number value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

bool sameDigest(const PhraseKey& key, const Digest& candidate)
{
  unsigned difference = 0;
  for (std::size_t index = 0; index < candidate.size(); ++index)
    difference |= static_cast<unsigned>(key.digest[index]) ^ static_cast<unsigned>(candidate[index]);
  return difference == 0;
}
}

std::vector<std::string> words(std::string_view text)
{
  const std::string fold = folded(text);
  std::vector<std::string> out;
  std::string current;
  const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(fold.data());
  const auto size = static_cast<utf8proc_ssize_t>(fold.size());
  utf8proc_ssize_t offset = 0;
  while (offset < size) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t read = utf8proc_iterate(bytes + offset, size - offset, &codepoint);
    if (read <= 0)
      return {};
    if (wordCodepoint(codepoint)) {
      current.append(fold, static_cast<std::size_t>(offset), static_cast<std::size_t>(read));
    }
    else if (!current.empty()) {
      out.push_back(std::move(current));
      current.clear();
    }
    offset += read;
  }
  if (!current.empty())
    out.push_back(std::move(current));
  return out;
}

Digest pbkdf2Sha256(const Pbkdf2Input& input)
{
  const Hmac hmac(input.password);
  std::vector<std::uint8_t> first;
  first.reserve(input.salt.size() + 4);
  std::ranges::transform(input.salt, std::back_inserter(first), [](char value) {
    return static_cast<std::uint8_t>(value);
  });
  first.insert(first.end(), {0x00, 0x00, 0x00, 0x01});
  Digest block = hmac.sign(first);
  Digest result = block;
  for (int round = 1; round < input.iterations; ++round) {
    block = hmac.sign(block);
    for (std::size_t index = 0; index < result.size(); ++index)
      result[index] =
          static_cast<std::uint8_t>(static_cast<unsigned>(result[index]) ^ static_cast<unsigned>(block[index]));
  }
  return result;
}

std::optional<PhraseKey> seal(const SealInput& input)
{
  const std::vector<std::string> parts = words(input.phrase);
  if (parts.size() < kMinWords || parts.size() > kMaxWords || input.salt.size() != kSaltBytes ||
      input.iterations < kMinIterations || input.iterations > kMaxIterations)
    return std::nullopt;
  return PhraseKey{.iterations = input.iterations,
                   .salt = std::string(input.salt),
                   .digest = pbkdf2Sha256(
                       {.password = joined(parts), .salt = input.salt, .iterations = input.iterations}),
                   .words = parts.size()};
}

std::string encode(const PhraseKey& key)
{
  const auto salt = std::span(reinterpret_cast<const std::uint8_t*>(key.salt.data()), key.salt.size());
  return std::string(kScheme) + "$" + std::to_string(key.iterations) + "$" + std::to_string(key.words) + "$" +
         hexOf(salt) + "$" + hexOf(key.digest);
}

std::optional<PhraseKey> decode(std::string_view stored)
{
  const std::vector<std::string_view> parts = fieldsOf(stored);
  if (parts.size() != kEncodedParts || parts[0] != kScheme)
    return std::nullopt;
  const auto iterations = numberOf<int>(parts[1]);
  const auto count = numberOf<std::size_t>(parts[2]);
  const auto salt = bytesOf(parts[3]);
  const auto digest = bytesOf(parts[4]);
  if (!iterations || *iterations < kMinIterations || *iterations > kMaxIterations || !count ||
      *count < kMinWords || *count > kMaxWords || !salt || salt->size() != kSaltBytes || !digest ||
      digest->size() != Digest{}.size())
    return std::nullopt;
  PhraseKey key{.iterations = *iterations, .salt = *salt, .digest = {}, .words = *count};
  std::ranges::transform(*digest, key.digest.begin(), [](char value) { return static_cast<std::uint8_t>(value); });
  return key;
}

bool spokenIn(const PhraseKey& key, std::string_view utterance)
{
  if (key.words < kMinWords || key.words > kMaxWords)
    return false;
  std::vector<std::string> said = words(utterance);
  if (said.size() > kMaxScreenedWords)
    said.erase(said.begin(), said.end() - static_cast<std::ptrdiff_t>(kMaxScreenedWords));
  for (std::size_t start = 0; start + key.words <= said.size(); ++start) {
    const std::string window = joined(std::span<const std::string>(said).subspan(start, key.words));
    if (sameDigest(key, pbkdf2Sha256({.password = window, .salt = key.salt, .iterations = key.iterations})))
      return true;
  }
  return false;
}

}
