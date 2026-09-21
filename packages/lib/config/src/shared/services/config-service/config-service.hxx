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

  // In-memory-only override consulted by every getter; never persisted back.
  static void setRuntimeString(const std::string& keyPath,
                               const std::string& value);

  static std::string getString(const std::string& keyPath);
  static int getInt(const std::string& keyPath);
  static bool getBool(const std::string& keyPath);
  static double getDouble(const std::string& keyPath);

  // True when the key exists in the loaded config (any type).
  static bool hasKey(const std::string& keyPath);

  // Updates the in-memory config and persists the value back (surgical edit); false when not persisted.
  static bool setBool(const std::string& keyPath, bool value);
  static bool setString(const std::string& keyPath, const std::string& value);
  static bool setInt(const std::string& keyPath, int value);
  static bool setDouble(const std::string& keyPath, double value);

  // Reads a TOML table (key = string scalar) into key/value pairs in file order.
  static std::vector<std::pair<std::string, std::string>>
  getStringPairs(const std::string& keyPath);

  static Json::Value drogonConfig();
};
