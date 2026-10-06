#include "proxy-allowlist.hxx"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{
constexpr int kIpv4Bits = 32;
constexpr int kIpv6Bits = 128;
constexpr int kByteBits = 8;
constexpr int kDecimalRadix = 10;
constexpr std::size_t kMappedPrefixBytes = 12;
constexpr std::array<unsigned char, kMappedPrefixBytes> kIpv4MappedPrefix{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

struct Address
{
  std::array<unsigned char, 16> bytes{};
  int bits{0};
};

struct Range
{
  Address base;
  int prefix{0};
};

std::string_view trimmed(std::string_view value)
{
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    value.remove_suffix(1);
  return value;
}

std::optional<Address> parseAddress(std::string_view text)
{
  const std::string terminated(text);
  Address address;
  if (inet_pton(AF_INET, terminated.c_str(), address.bytes.data()) == 1) {
    address.bits = kIpv4Bits;
    return address;
  }
  if (inet_pton(AF_INET6, terminated.c_str(), address.bytes.data()) != 1)
    return std::nullopt;
  if (std::ranges::equal(kIpv4MappedPrefix,
                         std::span(address.bytes).first(kMappedPrefixBytes))) {
    Address mapped;
    std::ranges::copy(std::span(address.bytes).subspan(kMappedPrefixBytes),
                      mapped.bytes.begin());
    mapped.bits = kIpv4Bits;
    return mapped;
  }
  address.bits = kIpv6Bits;
  return address;
}

std::optional<Range> parseRange(std::string_view entry)
{
  entry = trimmed(entry);
  if (entry.empty())
    return std::nullopt;
  const auto slash = entry.find('/');
  const auto address = parseAddress(entry.substr(0, slash));
  if (!address)
    return std::nullopt;
  Range range{.base = *address, .prefix = address->bits};
  if (slash == std::string_view::npos)
    return range;
  const std::string_view digits = entry.substr(slash + 1);
  if (digits.empty())
    return std::nullopt;
  range.prefix = 0;
  for (const char digit : digits) {
    if (digit < '0' || digit > '9')
      return std::nullopt;
    range.prefix = (range.prefix * kDecimalRadix) + (digit - '0');
    if (range.prefix > address->bits)
      return std::nullopt;
  }
  return range;
}

bool covers(const Range& range, const Address& address)
{
  if (range.base.bits != address.bits)
    return false;
  int remaining = range.prefix;
  for (std::size_t index = 0; remaining > 0; ++index) {
    const int taken = std::min(remaining, kByteBits);
    const auto mask = static_cast<unsigned char>(0xffU << (kByteBits - taken));
    if ((range.base.bytes.at(index) & mask) != (address.bytes.at(index) & mask))
      return false;
    remaining -= taken;
  }
  return true;
}

std::vector<Range> parseRanges(std::string_view configured)
{
  std::vector<Range> ranges;
  while (!configured.empty()) {
    const auto comma = configured.find(',');
    if (const auto range = parseRange(configured.substr(0, comma)))
      ranges.push_back(*range);
    if (comma == std::string_view::npos)
      break;
    configured.remove_prefix(comma + 1);
  }
  return ranges;
}

struct CachedRanges
{
  std::string text;
  std::vector<Range> ranges;
};

constexpr std::size_t kCachedLists = 8;

const std::vector<Range>& rangesOf(std::string_view configured)
{
  thread_local std::vector<CachedRanges> cache;
  const auto hit = std::ranges::find(cache, configured, &CachedRanges::text);
  if (hit != cache.end())
    return hit->ranges;
  if (cache.size() >= kCachedLists)
    cache.erase(cache.begin());
  cache.push_back({.text = std::string(configured),
                   .ranges = parseRanges(configured)});
  return cache.back().ranges;
}
}

bool proxy_allowlist::contains(const MembershipInput& input)
{
  const auto address = parseAddress(trimmed(input.address));
  if (!address)
    return false;
  return std::ranges::any_of(rangesOf(input.configured),
                             [&address](const Range& range) {
                               return covers(range, *address);
                             });
}

std::string proxy_allowlist::prefixOf(const PrefixInput& input)
{
  const std::string_view text = trimmed(input.address);
  auto address = parseAddress(text);
  if (!address)
    return std::string(text);
  const int prefix = std::clamp(
      address->bits == kIpv4Bits ? input.ipv4Bits : input.ipv6Bits, 0,
      address->bits);
  int remaining = prefix;
  for (auto& byte : address->bytes) {
    const int kept = std::clamp(remaining, 0, kByteBits);
    byte = static_cast<unsigned char>(
        kept == 0 ? 0U : static_cast<unsigned>(byte) & (0xffU << static_cast<unsigned>(kByteBits - kept)));
    remaining -= kept;
  }
  std::array<char, INET6_ADDRSTRLEN> buffer{};
  const int family = address->bits == kIpv4Bits ? AF_INET : AF_INET6;
  if (inet_ntop(family, address->bytes.data(), buffer.data(),
                static_cast<socklen_t>(buffer.size())) == nullptr)
    return std::string(text);
  return std::string(buffer.data()) + "/" + std::to_string(prefix);
}
