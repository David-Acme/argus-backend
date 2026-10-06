#pragma once

#include <feature/modules/schemas/module-catalog.hxx>
#include <json/value.h>

#include <optional>
#include <string>

struct ModuleCatalogParse
{
  std::optional<ModuleCatalog> catalog;
  std::string problem;
};

[[nodiscard]] ModuleCatalogParse parseModuleCatalog(const Json::Value& root);
[[nodiscard]] std::optional<ModuleCatalog> loadModuleCatalog(const std::string& path);
