#pragma once

#include <auth/user-role.hxx>
#include <feature/guard/vocabulary/maintenance-end-reason.hxx>
#include <feature/guard/vocabulary/maintenance-source.hxx>
#include <feature/guard/vocabulary/tamper-ask-answer.hxx>
#include <feature/guard/vocabulary/view-check-state.hxx>

#include <cstdint>
#include <json/value.h>
#include <string>
#include <vector>

struct ViewCheck
{
  int64_t cameraId{0};
  std::string cameraName;
  ViewCheckState state{ViewCheckState::Pending};
};

struct MaintenanceJsonInput
{
  bool owner{false};
  int64_t now{0};
};

struct MaintenanceWindow
{
  int64_t id{0};
  int64_t environmentId{0};
  std::vector<int64_t> cameraIds;
  int64_t declaredBy{0};
  UserRole declaredRole{UserRole::Unknown};
  std::string declaredName;
  MaintenanceSource source{MaintenanceSource::App};
  bool duress{false};
  int64_t startedAt{0};
  int64_t endsAt{0};
  int64_t endedAt{0};
  MaintenanceEndReason endReason{MaintenanceEndReason::None};
  std::vector<ViewCheck> viewChecks;

  [[nodiscard]] bool active(int64_t now) const;
  [[nodiscard]] bool covers(int64_t cameraId) const;
  [[nodiscard]] Json::Value toJson(const MaintenanceJsonInput& input) const;
};

struct TamperAsk
{
  int64_t id{0};
  int64_t cameraId{0};
  int64_t environmentId{0};
  int64_t onsetAt{0};
  int64_t askedAt{0};
  std::vector<int64_t> askedUsers;
  TamperAskAnswer answer{TamperAskAnswer::Pending};
  int64_t answeredBy{0};
  int64_t answeredAt{0};
};

struct MaintenanceEndedEvent
{
  const MaintenanceWindow& window;
  std::vector<std::string> cameraNames;
  std::string environmentName;
};

namespace maintenance_schema
{
[[nodiscard]] std::string endedEventId(int64_t windowId);
[[nodiscard]] Json::Value endedEventJson(const MaintenanceEndedEvent& event);
[[nodiscard]] std::string idsToJson(const std::vector<int64_t>& ids);
[[nodiscard]] std::vector<int64_t> idsFromJson(const std::string& text);
[[nodiscard]] std::string viewChecksToJson(const std::vector<ViewCheck>& checks);
[[nodiscard]] std::vector<ViewCheck> viewChecksFromJson(const std::string& text);
}
