#pragma once

#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <sstream>
#include <string>

namespace json_util
{
inline std::string toString(const Json::Value& value)
{
  if (value.isNull())
    return "{}";
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

inline Json::Value fromString(const std::string& raw)
{
  if (raw.empty())
    return Json::Value();
  std::istringstream in(raw);
  Json::Value value;
  Json::CharReaderBuilder builder;
  std::string errors;
  if (!Json::parseFromStream(builder, in, &value, &errors))
    return Json::Value();
  return value;
}
} // namespace json_util
