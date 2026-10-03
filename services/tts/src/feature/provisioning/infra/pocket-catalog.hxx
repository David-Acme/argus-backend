#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct PocketCatalogVariant
{
  std::string name;
  std::uint64_t bytes{0};
};

struct PocketCatalogVoice
{
  std::string variant;
  std::string name;
  std::uint64_t bytes{0};
  std::string license;

  [[nodiscard]] bool nonCommercial() const;
};

struct PocketCatalog
{
  std::vector<PocketCatalogVariant> variants;
  std::vector<PocketCatalogVoice> voices;

  [[nodiscard]] const PocketCatalogVariant* variant(std::string_view name) const;
  [[nodiscard]] const PocketCatalogVoice* voice(std::string_view variantName, std::string_view name) const;

  [[nodiscard]] static PocketCatalog parse(std::string_view text);
  [[nodiscard]] static PocketCatalog load(const std::filesystem::path& file);
};

[[nodiscard]] bool isComponentToken(std::string_view token);
