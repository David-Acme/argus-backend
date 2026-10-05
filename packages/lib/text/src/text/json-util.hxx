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

inline bool isValid(const std::string& raw)
{
  if (raw.empty())
    return false;
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  try {
    return reader->parse(raw.data(), raw.data() + raw.size(), &value, &errors);
  }
  catch (const Json::Exception&) {
    return false;
  }
}

inline Json::Value fromString(const std::string& raw)
{
  if (raw.empty())
    return Json::Value();
  std::istringstream in(raw);
  Json::Value value;
  Json::CharReaderBuilder builder;
  std::string errors;
  try {
    if (!Json::parseFromStream(builder, in, &value, &errors))
      return {};
  }
  catch (const Json::Exception&) {
    return {};
  }
  return value;
}
}
