#pragma once

#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
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

// True only when the payload is valid JSON; empty is not valid here so
// callers keep treating empty as "absent".
inline bool isValid(const std::string& raw)
{
  if (raw.empty())
    return false;
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  return reader->parse(raw.data(), raw.data() + raw.size(), &value, &errors);
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
