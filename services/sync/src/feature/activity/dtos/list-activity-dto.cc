#include "list-activity-dto.hxx"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>
#include <sync/user-action-event.hxx>
#include <validation/validation_dsl.hxx>

namespace
{
constexpr int64_t kInvalidNumber = -1;
constexpr std::size_t kMaxTableBytes = 32;
constexpr std::size_t kMaxCursorBytes = 40;

std::optional<std::string> textOf(const drogon::HttpRequestPtr& request, const char* name)
{
  const std::string& value = request->getParameter(name);
  if (value.empty())
    return std::nullopt;
  return value;
}

std::optional<int64_t> numberOf(const drogon::HttpRequestPtr& request, const char* name)
{
  const std::string& value = request->getParameter(name);
  if (value.empty())
    return std::nullopt;
  int64_t result{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size())
    return kInvalidNumber;
  return result;
}

bool tableCharacter(char c)
{
  return (c >= 'a' && c <= 'z') || c == '_';
}

bool cursorCharacter(char c)
{
  return (c >= '0' && c <= '9') || c == '-';
}

std::optional<std::string> checkedPositive(const std::optional<int64_t>& value, const char* message)
{
  if (value && *value <= 0)
    return std::string(message);
  return std::nullopt;
}
}

ListActivityDto ListActivityDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  ListActivityDto dto;
  dto.module = textOf(request, "module");
  dto.action = textOf(request, "action");
  dto.table = textOf(request, "table");
  dto.userId = numberOf(request, "userId");
  dto.from = numberOf(request, "from");
  dto.to = numberOf(request, "to");
  dto.cursor = textOf(request, "cursor");
  if (const auto limit = numberOf(request, "limit"))
    dto.limit = *limit > kMaxLimit ? kMaxLimit : static_cast<int>(*limit);

  START_VALIDATION(ListActivityDto, dto)
  IS_IN_OPTIONAL(action, "create", "read", "update", "delete")
  IS_POSITIVE(limit)
  CUSTOM_LAMBDA(module, [](const ListActivityDto& d) -> std::optional<std::string> {
    if (d.module && !user_action_event::isModuleId(*d.module))
      return "module must be a lowercase module id";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(table, [](const ListActivityDto& d) -> std::optional<std::string> {
    if (d.table && (d.table->size() > kMaxTableBytes || !std::ranges::all_of(*d.table, tableCharacter)))
      return "table must be a table name";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(userId, [](const ListActivityDto& d) -> std::optional<std::string> {
    return checkedPositive(d.userId, "userId must be a positive number");
  })
  CUSTOM_LAMBDA(from, [](const ListActivityDto& d) -> std::optional<std::string> {
    return checkedPositive(d.from, "from must be a positive unix time");
  })
  CUSTOM_LAMBDA(to, [](const ListActivityDto& d) -> std::optional<std::string> {
    if (const auto positive = checkedPositive(d.to, "to must be a positive unix time"))
      return positive;
    if (d.from && d.to && *d.from > *d.to)
      return "to must not be before from";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(cursor, [](const ListActivityDto& d) -> std::optional<std::string> {
    if (d.cursor && (d.cursor->size() > kMaxCursorBytes || !std::ranges::all_of(*d.cursor, cursorCharacter)))
      return "cursor is not one this endpoint issued";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
