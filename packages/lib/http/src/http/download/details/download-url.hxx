#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace file_download::details
{

struct UrlParts
{
  std::string origin;
  std::string target;

  friend bool operator==(const UrlParts&, const UrlParts&) = default;
};

struct LocationInput
{
  std::string_view base;
  std::string_view location;
};

struct ContentRange
{
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  std::optional<std::uint64_t> total;

  friend bool operator==(const ContentRange&, const ContentRange&) = default;
};

[[nodiscard]] std::optional<UrlParts> splitUrl(std::string_view url);

[[nodiscard]] std::optional<std::string> resolveLocation(const LocationInput& input);

[[nodiscard]] std::optional<ContentRange> parseContentRange(std::string_view header);

}
