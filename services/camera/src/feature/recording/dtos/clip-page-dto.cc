#include "clip-page-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace
{
std::optional<int64_t> parseNumber(const std::string& text)
{
  if (text.empty() ||
      !std::ranges::all_of(text, [](unsigned char c) { return std::isdigit(c) != 0; }))
    return std::nullopt;
  try {
    return std::stoll(text);
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}
}

ClipPageDto ClipPageDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  ClipPageDto dto;
  const auto before = parseNumber(request->getParameter("before"));
  const auto beforeId = parseNumber(request->getParameter("beforeId"));
  const auto limit = parseNumber(request->getParameter("limit"));
  dto.beforeCreatedAt = before.value_or(0);
  dto.beforeId = beforeId.value_or(0);
  dto.limit = limit.value_or(50);

  START_VALIDATION(ClipPageDto, dto)
  IS_NON_NEGATIVE(beforeCreatedAt)
  IS_NON_NEGATIVE(beforeId)
  IS_POSITIVE(limit)
  MAX_INT(limit, 200)
  END_VALIDATION()
  return dto;
}
