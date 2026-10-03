#include "pocket-catalog.hxx"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>

namespace
{
constexpr std::size_t kMaxTokenLength = 32;

std::optional<std::uint64_t> bytesOf(std::string_view text)
{
  const std::string digits(text);
  std::uint64_t value = 0;
  const auto* last = digits.data() + digits.size();
  const auto [end, error] = std::from_chars(digits.data(), last, value);
  if (digits.empty() || error != std::errc{} || end != last || value == 0)
    return std::nullopt;
  return value;
}

bool isLicenseToken(std::string_view token)
{
  return !token.empty() && token.size() <= kMaxTokenLength && std::ranges::all_of(token, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
  });
}

std::vector<std::string_view> fieldsOf(std::string_view line)
{
  std::vector<std::string_view> fields;
  for (const auto word : line | std::views::split(' ')) {
    const std::string_view field(word.begin(), word.end());
    if (!field.empty())
      fields.push_back(field);
  }
  return fields;
}
}

bool isComponentToken(std::string_view token)
{
  return !token.empty() && token.size() <= kMaxTokenLength && std::ranges::all_of(token, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

bool PocketCatalogVoice::nonCommercial() const
{
  return license.find("-nc") != std::string::npos;
}

const PocketCatalogVariant* PocketCatalog::variant(std::string_view name) const
{
  const auto found = std::ranges::find(variants, name, &PocketCatalogVariant::name);
  return found == variants.end() ? nullptr : &*found;
}

const PocketCatalogVoice* PocketCatalog::voice(std::string_view variantName, std::string_view name) const
{
  const auto found = std::ranges::find_if(
      voices, [&](const PocketCatalogVoice& voice) { return voice.variant == variantName && voice.name == name; });
  return found == voices.end() ? nullptr : &*found;
}

PocketCatalog PocketCatalog::parse(std::string_view text)
{
  PocketCatalog catalog;
  for (const auto row : text | std::views::split('\n')) {
    const auto fields = fieldsOf(std::string_view(row.begin(), row.end()));
    if (fields.size() == 3 && fields[0] == "variant" && isComponentToken(fields[1])) {
      if (const auto bytes = bytesOf(fields[2]))
        catalog.variants.push_back({.name = std::string(fields[1]), .bytes = *bytes});
    }
    else if (fields.size() == 5 && fields[0] == "voice" && isComponentToken(fields[1]) &&
             isComponentToken(fields[2]) && isLicenseToken(fields[4])) {
      if (const auto bytes = bytesOf(fields[3]))
        catalog.voices.push_back({.variant = std::string(fields[1]),
                                  .name = std::string(fields[2]),
                                  .bytes = *bytes,
                                  .license = std::string(fields[4])});
    }
  }
  return catalog;
}

PocketCatalog PocketCatalog::load(const std::filesystem::path& file)
{
  std::ifstream input(file);
  if (!input)
    return {};
  const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  return parse(text);
}
