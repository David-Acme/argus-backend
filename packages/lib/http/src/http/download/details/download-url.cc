#include "download-url.hxx"

#include <charconv>
#include <memory>

namespace file_download::details
{
namespace
{

constexpr std::string_view kHttp = "http://";
constexpr std::string_view kHttps = "https://";

std::optional<std::uint64_t> parseNumber(std::string_view text)
{
  std::uint64_t value = 0;
  const auto* const begin = std::to_address(text.begin());
  const auto* const end = std::to_address(text.end());
  const auto [ptr, error] = std::from_chars(begin, end, value);
  if (text.empty() || error != std::errc{} || ptr != end)
    return std::nullopt;
  return value;
}

std::string_view withoutFragment(std::string_view text)
{
  return text.substr(0, text.find('#'));
}

std::string_view trimmed(std::string_view text)
{
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string_view::npos)
    return {};
  const auto last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

bool hasOtherScheme(std::string_view location)
{
  const auto colon = location.find(':');
  if (colon == std::string_view::npos || colon == 0 ||
      location.substr(0, colon).find_first_of("/?#") != std::string_view::npos)
    return false;
  return true;
}

}

std::optional<UrlParts> splitUrl(std::string_view url)
{
  url = withoutFragment(url);
  std::size_t schemeLength = 0;
  if (url.starts_with(kHttps))
    schemeLength = kHttps.size();
  else if (url.starts_with(kHttp))
    schemeLength = kHttp.size();
  else
    return std::nullopt;
  const auto authorityEnd = url.find_first_of("/?", schemeLength);
  const auto authority = url.substr(
      schemeLength, authorityEnd == std::string_view::npos
                        ? std::string_view::npos
                        : authorityEnd - schemeLength);
  if (authority.empty() || authority.find_first_of(" @") != std::string_view::npos)
    return std::nullopt;
  UrlParts parts{.origin = std::string(url.substr(0, schemeLength + authority.size())),
                 .target = "/"};
  if (authorityEnd != std::string_view::npos)
  {
    const auto rest = url.substr(authorityEnd);
    parts.target = rest.starts_with('?') ? "/" + std::string(rest) : std::string(rest);
  }
  return parts;
}

std::optional<std::string> resolveLocation(const LocationInput& input)
{
  const auto location = withoutFragment(trimmed(input.location));
  if (location.empty())
    return std::nullopt;
  if (location.starts_with(kHttp) || location.starts_with(kHttps))
    return splitUrl(location) ? std::optional<std::string>(location) : std::nullopt;
  if (hasOtherScheme(location))
    return std::nullopt;
  const auto base = splitUrl(input.base);
  if (!base)
    return std::nullopt;
  if (location.starts_with("//"))
  {
    const auto scheme = base->origin.substr(0, base->origin.find("//"));
    std::string absolute = scheme + std::string(location);
    return splitUrl(absolute) ? std::optional<std::string>(std::move(absolute)) : std::nullopt;
  }
  if (location.starts_with('/'))
    return base->origin + std::string(location);
  const auto path = std::string_view(base->target).substr(0, base->target.find('?'));
  const auto directory = path.substr(0, path.rfind('/') + 1);
  return base->origin + std::string(directory) + std::string(location);
}

std::optional<ContentRange> parseContentRange(std::string_view header)
{
  header = trimmed(header);
  constexpr std::string_view kUnit = "bytes ";
  if (!header.starts_with(kUnit))
    return std::nullopt;
  header.remove_prefix(kUnit.size());
  const auto dash = header.find('-');
  const auto slash = header.find('/');
  if (dash == std::string_view::npos || slash == std::string_view::npos || dash > slash)
    return std::nullopt;
  const auto first = parseNumber(header.substr(0, dash));
  const auto last = parseNumber(header.substr(dash + 1, slash - dash - 1));
  if (!first || !last || *last < *first)
    return std::nullopt;
  ContentRange range{.first = *first, .last = *last, .total = std::nullopt};
  const auto total = header.substr(slash + 1);
  if (total != "*")
  {
    range.total = parseNumber(total);
    if (!range.total || *range.total <= *last)
      return std::nullopt;
  }
  return range;
}

}
