#include "list-decisions-dto.hxx"

#include <charconv>
#include <shared/enums.hxx>
#include <shared/validation/validation_dsl.hxx>
#include <string>
#include <system_error>

namespace
{
constexpr int kMaxDecisionLimit = 200;

int parseInt(const std::string& value, int fallback)
{
  int parsed = fallback;
  if (!value.empty()) {
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size())
      parsed = -1;
  }
  return parsed;
}

int64_t parseInt64(const std::string& value, int64_t fallback)
{
  int64_t parsed = fallback;
  if (!value.empty()) {
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size())
      parsed = -1;
  }
  return parsed;
}
} // namespace

ListDecisionsDto ListDecisionsDto::fromRequest(
    const drogon::HttpRequestPtr& request)
{
  ListDecisionsDto dto;
  dto.limit = parseInt(request->getParameter("limit"), 20);
  if (dto.limit > kMaxDecisionLimit)
    dto.limit = kMaxDecisionLimit;
  dto.from = parseInt64(request->getParameter("from"), 0);
  dto.to = parseInt64(request->getParameter("to"), 0);
  dto.cameraId = parseInt64(request->getParameter("camera_id"), 0);
  dto.severity = request->getParameter("severity");
  dto.decisionMode = request->getParameter("decision_mode");
  dto.suppressionReason = request->getParameter("suppression_reason");
  dto.divergentOnly = request->getParameter("divergent_only") == "true" ||
                      request->getParameter("divergent_only") == "1";
  dto.afterCreatedAt =
      parseInt64(request->getParameter("after_created_at"), 0);
  dto.afterEventId = request->getParameter("after_event_id");
  dto.nearMissMargin = parseInt(request->getParameter("near_miss_margin"), 0);

  START_VALIDATION(ListDecisionsDto, dto)
  IS_POSITIVE(limit)
  IS_NON_NEGATIVE(from)
  IS_NON_NEGATIVE(to)
  IS_NON_NEGATIVE(cameraId)
  IS_NON_NEGATIVE(afterCreatedAt)
  IS_NON_NEGATIVE(nearMissMargin)
  END_VALIDATION()
  if (!dto.severity.empty() &&
      guardDangerFromString(dto.severity) == GuardDanger::None &&
      dto.severity != "none")
    throw ValidationException({{"severity", {"unknown severity"}}}, 422);
  if (!dto.decisionMode.empty() && dto.decisionMode != "shadow" &&
      dto.decisionMode != "enforce")
    throw ValidationException({{"decision_mode", {"unknown decision mode"}}},
                              422);
  if (!dto.suppressionReason.empty() &&
      !decisionSuppressionFromString(dto.suppressionReason).has_value())
    throw ValidationException(
        {{"suppression_reason", {"unknown suppression reason"}}}, 422);
  if (dto.afterEventId.empty() != (dto.afterCreatedAt == 0))
    throw ValidationException(
        {{"after_event_id", {"cursor needs after_created_at and after_event_id together"}}},
        422);
  return dto;
}
