#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Json
{
class Value;
}

class ConfigService
{
public:
  static void load(const std::string& path);
  static void loadOverlay(const std::string& path);

  static void setRuntimeString(const std::string& keyPath,
                               const std::string& value);

  static std::string getString(const std::string& keyPath);
  static int getInt(const std::string& keyPath);
  static bool getBool(const std::string& keyPath);
  static double getDouble(const std::string& keyPath);

  static bool hasKey(const std::string& keyPath);

  static std::string path();

  static bool setBool(const std::string& keyPath, bool value);
  static bool setString(const std::string& keyPath, const std::string& value);
  static bool setInt(const std::string& keyPath, int value);
  static bool setDouble(const std::string& keyPath, double value);

  static std::vector<std::pair<std::string, std::string>>
  getStringPairs(const std::string& keyPath);

  static Json::Value drogonConfig();
};
