#include "tapo-recordings.hxx"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace
{

constexpr std::size_t kDeviceDateLength = 8;
constexpr std::size_t kIsoDateLength = 10;

bool digits(std::string_view text)
{
  return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

const Json::Value* entryOf(const Json::Value& element, const char* field)
{
  if (!element.isObject())
    return nullptr;
  if (element.isMember(field))
    return &element;
  for (const auto& name : element.getMemberNames()) {
    const Json::Value& inner = element[name];
    if (inner.isObject() && inner.isMember(field))
      return &inner;
  }
  return nullptr;
}

std::optional<int64_t> secondsOf(const Json::Value& value)
{
  if (value.isIntegral())
    return value.asInt64();
  if (!value.isString())
    return std::nullopt;
  const std::string text = value.asString();
  int64_t parsed = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return parsed;
}

}

namespace tapo_recordings
{

std::optional<std::string> deviceDate(std::string_view isoDate)
{
  if (isoDate.size() != kIsoDateLength || isoDate[4] != '-' || isoDate[7] != '-')
    return std::nullopt;
  std::string compact;
  compact.reserve(kDeviceDateLength);
  compact.append(isoDate.substr(0, 4));
  compact.append(isoDate.substr(5, 2));
  compact.append(isoDate.substr(8, 2));
  if (!digits(compact))
    return std::nullopt;
  return compact;
}

Json::Value days(const Json::Value& searchResults)
{
  std::set<std::string> found;
  if (searchResults.isArray()) {
    for (const auto& element : searchResults) {
      const Json::Value* entry = entryOf(element, "date");
      if (entry == nullptr || !(*entry)["date"].isString())
        continue;
      const std::string date = (*entry)["date"].asString();
      if (date.size() != kDeviceDateLength || !digits(date))
        continue;
      found.insert(date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2));
    }
  }
  Json::Value out(Json::arrayValue);
  for (const auto& date : found)
    out.append(date);
  return out;
}

Json::Value segments(const Json::Value& videoResults)
{
  std::vector<std::pair<int64_t, int64_t>> found;
  if (videoResults.isArray()) {
    for (const auto& element : videoResults) {
      const Json::Value* entry = entryOf(element, "startTime");
      if (entry == nullptr)
        continue;
      const auto start = secondsOf((*entry)["startTime"]);
      const auto end = secondsOf((*entry)["endTime"]);
      if (!start || !end || *start <= 0 || *end < *start)
        continue;
      found.emplace_back(*start, *end);
    }
  }
  std::ranges::sort(found);
  Json::Value out(Json::arrayValue);
  for (const auto& [start, end] : found) {
    Json::Value segment(Json::objectValue);
    segment["start"] = static_cast<Json::Int64>(start);
    segment["end"] = static_cast<Json::Int64>(end);
    out.append(std::move(segment));
  }
  return out;
}

}
