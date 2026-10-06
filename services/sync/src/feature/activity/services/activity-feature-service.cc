#include "activity-feature-service.hxx"

#include <charconv>
#include <errors/validation-exception.hxx>
#include <string_view>
#include <system_error>

namespace
{
constexpr char kSeparator = '-';

std::optional<int64_t> numberIn(std::string_view text)
{
  int64_t value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < 0)
    return std::nullopt;
  return value;
}
}

std::string activity_cursor::encode(const ActivityCursor& cursor)
{
  return std::to_string(cursor.createdAt) + kSeparator + std::to_string(cursor.id);
}

std::optional<ActivityCursor> activity_cursor::decode(const std::string& text)
{
  const auto split = text.find(kSeparator);
  if (split == std::string::npos)
    return std::nullopt;
  const std::string_view view(text);
  const auto createdAt = numberIn(view.substr(0, split));
  const auto id = numberIn(view.substr(split + 1));
  if (!createdAt || !id)
    return std::nullopt;
  return ActivityCursor{.createdAt = *createdAt, .id = *id};
}

drogon::Task<ActivityPage> ActivityFeatureService::list(const ListActivityDto& query) const
{
  std::optional<ActivityCursor> after;
  if (query.cursor) {
    after = activity_cursor::decode(*query.cursor);
    if (!after)
      throw ValidationException({{"cursor", {"cursor is not one this endpoint issued"}}});
  }

  ActivityPage page;
  page.items = co_await repository_.list(
      {.filter = {.module = query.module.value_or(""),
                  .action = query.action.value_or(""),
                  .table = query.table.value_or(""),
                  .userId = query.userId.value_or(0),
                  .from = query.from.value_or(0),
                  .to = query.to.value_or(0)},
       .after = after,
       .limit = query.limit + 1});

  if (std::cmp_greater(page.items.size(), query.limit)) {
    page.items.resize(static_cast<std::size_t>(query.limit));
    const auto& last = page.items.back();
    page.nextCursor = activity_cursor::encode({.createdAt = last.createdAt, .id = last.id});
  }
  co_return page;
}
