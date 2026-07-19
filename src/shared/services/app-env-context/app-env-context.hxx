#pragma once

#include <optional>
#include <string>
#include <unordered_map>

class AppEnvContext
{
public:
  static void load(const std::string& path);
  static std::string get(const std::string& key);
  static std::optional<std::string> getOptional(const std::string& key);

private:
  static std::unordered_map<std::string, std::string> values_;
};
