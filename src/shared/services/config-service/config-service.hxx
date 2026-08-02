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

  static std::string getString(const std::string& keyPath);
  static int getInt(const std::string& keyPath);
  static bool getBool(const std::string& keyPath);
  static double getDouble(const std::string& keyPath);

  // Runtime mutations: update the in-memory config AND persist the value back
  // to the source file (surgical edit, comments/formatting preserved). Return
  // false if the file could not be loaded/patched (value not persisted).
  static bool setBool(const std::string& keyPath, bool value);
  static bool setString(const std::string& keyPath, const std::string& value);
  static bool setInt(const std::string& keyPath, int value);
  static bool setDouble(const std::string& keyPath, double value);

  // Reads a TOML table (key = string scalar) into key/value pairs, in file
  // order. Missing/non-table/non-scalar nodes yield an empty vector.
  static std::vector<std::pair<std::string, std::string>>
  getStringPairs(const std::string& keyPath);

  static Json::Value drogonConfig();
};
