#pragma once

#include <text/json-util.hxx>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

struct CameraStreamPaths
{
  std::string main;
  std::string sub;
};

namespace camera_stream_paths
{
inline constexpr const char* kMainKey = "streamPath";
inline constexpr const char* kSubKey = "subStreamPath";
inline constexpr const char* kCatalogKey = "catalogId";
inline constexpr const char* kRetentionIncidentKey = "retentionIncident";
inline constexpr size_t kMaxCatalogIdLength = 40;
inline constexpr const char* kDefaultMain = "/stream1";
inline constexpr const char* kDefaultSub = "/stream2";
inline constexpr size_t kMaxLength = 200;

inline bool isValid(std::string_view path)
{
  if (path.empty())
    return true;
  if (path.size() > kMaxLength || path.front() != '/')
    return false;
  constexpr std::string_view kPunctuation = "/._~-?=&%+,;:";
  return std::ranges::all_of(path, [kPunctuation](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           kPunctuation.find(c) != std::string_view::npos;
  });
}

inline CameraStreamPaths of(const std::string& config)
{
  const Json::Value json = json_util::fromString(config);
  const auto read = [&json](const char* key, const char* fallback) {
    if (!json.isObject() || !json[key].isString())
      return std::string(fallback);
    std::string value = json[key].asString();
    return value.empty() || !isValid(value) ? std::string(fallback) : value;
  };
  return {.main = read(kMainKey, kDefaultMain), .sub = read(kSubKey, kDefaultSub)};
}

inline std::string catalogIdOf(const std::string& config)
{
  const Json::Value json = json_util::fromString(config);
  if (!json.isObject() || !json[kCatalogKey].isString())
    return {};
  return json[kCatalogKey].asString();
}

inline bool retentionIncidentOf(const std::string& config)
{
  const Json::Value json = json_util::fromString(config);
  return json.isObject() && json[kRetentionIncidentKey].isBool() &&
         json[kRetentionIncidentKey].asBool();
}

struct ConfigChange
{
  std::string config;
  std::optional<std::string> main;
  std::optional<std::string> sub;
  std::optional<std::string> catalogId;
  std::optional<bool> retentionIncident;
};

inline std::string withConfig(const ConfigChange& change)
{
  Json::Value json = json_util::fromString(change.config);
  if (!json.isObject())
    json = Json::Value(Json::objectValue);
  const auto apply = [&json](const char* key, const std::optional<std::string>& value) {
    if (!value)
      return;
    if (value->empty())
      json.removeMember(key);
    else
      json[key] = *value;
  };
  apply(kMainKey, change.main);
  apply(kSubKey, change.sub);
  apply(kCatalogKey, change.catalogId);
  if (change.retentionIncident) {
    if (*change.retentionIncident)
      json[kRetentionIncidentKey] = true;
    else
      json.removeMember(kRetentionIncidentKey);
  }
  return json_util::toString(json);
}
}
