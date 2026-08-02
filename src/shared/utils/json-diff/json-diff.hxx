#pragma once

#include <json/value.h>
#include <string>
#include <unordered_map>
#include <vector>

struct Change
{
  Json::Value previous;
  Json::Value current;
};

using ChangesDiff = std::unordered_map<std::string, Change>;

struct ChangesComparisonResult
{
  std::string type;
  ChangesDiff changes;
};

class JsonDiff
{
public:
  static ChangesDiff createFlatDiff(const Json::Value& original,
                                    const Json::Value& updated);
  static ChangesComparisonResult
  compareChanges(const ChangesDiff& prevChanges,
                 const ChangesDiff& startChanges);
  static Json::Value compareObjects(const Json::Value& left,
                                    const Json::Value& right);
  static Json::Value applyChanges(const ChangesDiff& changes,
                                  Json::Value target);
  static ChangesDiff fromJsonString(const std::string& jsonStr);
  static Json::Value toJson(const ChangesDiff& changes);

private:
  static void diffRecursive(const Json::Value& orig, const Json::Value& upd,
                            std::string& path, ChangesDiff& out);
  static bool valuesEqual(const Json::Value& a, const Json::Value& b);
  static Json::Value getByPath(const Json::Value& root,
                               const std::vector<std::string>& segments);
  static void setByPath(Json::Value& root,
                        const std::vector<std::string>& segments,
                        const Json::Value& value);
  static std::vector<std::string> splitPath(const std::string& path);
  static void deepMerge(Json::Value& target, const Json::Value& source);
  static bool isIndex(const std::string& s);
};
