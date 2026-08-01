#pragma once

#include <string>

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

  static Json::Value drogonConfig();
};
