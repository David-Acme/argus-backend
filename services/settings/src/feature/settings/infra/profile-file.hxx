#pragma once

#include <feature/settings/services/settings-profile.hxx>
#include <json/value.h>

#include <optional>
#include <string>

struct ProfileFileParse
{
  std::optional<ProfileCatalog> catalog;
  std::string problem;
};

[[nodiscard]] ProfileFileParse parseProfileCatalog(const Json::Value& root);
[[nodiscard]] std::optional<ProfileCatalog> loadProfileFile(const std::string& path);
