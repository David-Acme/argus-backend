#include "config-service.hxx"

#include <drogon/drogon.h>
#include <json/reader.h>
#include <json/value.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <toml++/toml.hpp>

namespace
{

std::optional<toml::table> gConfig;

const toml::node* resolvePath(const std::string& keyPath)
{
  if (!gConfig)
    return nullptr;

  const toml::node* current = &*gConfig;
  std::stringstream ss(keyPath);
  std::string segment;
  while (std::getline(ss, segment, '.')) {
    if (!current->is_table())
      return nullptr;
    auto& tbl = *current->as_table();
    auto it = tbl.find(segment);
    if (it == tbl.end())
      return nullptr;
    current = &it->second;
  }
  return current;
}

template <typename T>
T getValue(const std::string& keyPath, T defaultVal)
{
  const auto* node = resolvePath(keyPath);
  if (!node)
    return defaultVal;
  if constexpr (std::is_same_v<T, int>)
    return static_cast<int>(node->value_or(static_cast<int64_t>(defaultVal)));
  else if constexpr (std::is_same_v<T, bool>)
    return static_cast<bool>(node->value_or(static_cast<bool>(defaultVal)));
  else
    return node->value_or(defaultVal);
}

} // namespace

void ConfigService::load(const std::string& path)
{
  try {
    gConfig = toml::parse_file(path);
    LOG_INFO << "Configuration loaded from " << path;
  }
  catch (const toml::parse_error& e) {
    auto& src = e.source();
    std::string detail{e.description()};
    std::string filePath =
        src.path ? std::string{*src.path} : std::string("(unknown)");
    LOG_FATAL << "Failed to parse config " << path << ": " << detail << " at "
              << filePath << ":" << src.begin.line << ":" << src.begin.column;
    throw std::runtime_error("ConfigService: failed to parse " + path);
  }
}

std::string ConfigService::getString(const std::string& keyPath)
{
  return getValue<std::string>(keyPath, "");
}

int ConfigService::getInt(const std::string& keyPath)
{
  return getValue<int>(keyPath, 0);
}

bool ConfigService::getBool(const std::string& keyPath)
{
  return getValue<bool>(keyPath, false);
}

double ConfigService::getDouble(const std::string& keyPath)
{
  return getValue<double>(keyPath, 0.0);
}

Json::Value ConfigService::drogonConfig()
{
  if (!gConfig)
    throw std::runtime_error("ConfigService: not loaded");

  auto drogonNode = gConfig->at_path("drogon");
  auto* drogonTbl = drogonNode.as_table();
  if (!drogonTbl)
    return Json::Value{};

  std::stringstream ss;
  ss << toml::json_formatter{*drogonTbl};

  Json::Value root;
  Json::Reader reader;
  if (!reader.parse(ss.str(), root)) {
    LOG_ERROR << "Failed to parse Drogon config JSON: "
              << reader.getFormattedErrorMessages();
    return Json::Value{};
  }
  return root;
}
