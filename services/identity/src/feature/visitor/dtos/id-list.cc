#include "id-list.hxx"

#include <algorithm>
#include <limits>

std::vector<int64_t> visitor_dto::idsOf(const Json::Value& array)
{
  std::vector<int64_t> ids;
  if (!array.isArray())
    return ids;
  ids.reserve(array.size());
  for (const auto& id : array) {
    if (id.isInt64() && id.asInt64() > 0)
      ids.push_back(id.asInt64());
    else if (id.isUInt64() &&
             id.asUInt64() <= static_cast<Json::UInt64>(
                                  std::numeric_limits<int64_t>::max()))
      ids.push_back(static_cast<int64_t>(id.asUInt64()));
  }
  std::ranges::sort(ids);
  const auto duplicates = std::ranges::unique(ids);
  ids.erase(duplicates.begin(), duplicates.end());
  return ids;
}
