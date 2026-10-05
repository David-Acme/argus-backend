#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <ctime>
#include <feature/guard/guard-schedule.hxx>
#include <feature/guard/repositories/response/response-query.hxx>
#include <feature/guard/vocabulary/recipient-mode.hxx>
#include <feature/guard/vocabulary/response-strategy.hxx>
#include <json/value.h>
#include <optional>
#include <shared/vocabulary/presence-state.hxx>
#include <span>
#include <string>
#include <vector>

struct ResponseUser
{
  int64_t userId{0};
  UserRole role{UserRole::Guest};
  bool active{true};
  std::string name;
  std::string lang;
};

struct ResponseMember
{
  int64_t userId{0};
  UserRole role{UserRole::Guest};
  std::string name;
  RecipientMode mode{RecipientMode::Off};
  int step{0};
  bool onDuty{false};
  bool customized{false};
};

struct ResponseMembersInput
{
  std::span<const ResponseUser> users;
  std::span<const ResponseRecipientRow> rows;
};

struct ResponsePlanInput
{
  int64_t environmentId{0};
  std::span<const ResponseMember> members;
  std::span<const PresenceRow> presence;
  std::span<const int64_t> excluded;
  ResponseTrigger trigger{ResponseTrigger::Intrusion};
  bool indoor{false};
  bool night{false};
  bool clearThreat{false};
  bool staffedNow{false};
  bool hasCamera{false};
  int stepSeconds{45};
  std::string emergencyNumber;
  std::span<const ResponseContactRow> contacts;
};

struct ResponsePlanEntry
{
  int64_t userId{0};
  int step{0};
  RecipientMode mode{RecipientMode::Call};
  bool mandatory{false};
  bool discreet{false};
};

struct ResponsePlan
{
  int64_t environmentId{0};
  ResponseStrategy strategy{ResponseStrategy::Ordered};
  std::vector<ResponsePlanEntry> entries;
  int stepCount{0};
  int stepSeconds{45};
  bool offerCamera{false};
  bool offerSiren{false};
  std::string emergencyNumber;
  std::vector<ResponseContactRow> contacts;
};

namespace response_plan
{
inline constexpr int kMaxStep = 32;

std::vector<ResponseMember> members(const ResponseMembersInput& input);

ResponsePlan build(const ResponsePlanInput& input);

std::vector<int64_t> stepUsers(const ResponsePlan& plan, int step);

struct AudienceInput
{
  std::span<const ResponseMember> members;
  std::span<const int64_t> excluded;
};

std::vector<int64_t> audience(const AudienceInput& input);

Json::Value toJson(const ResponsePlan& plan);

bool staffedAt(const GuardSchedule& schedule, const std::tm& local);
}

class ResponseDirectory
{
public:
  virtual ~ResponseDirectory() = default;

  [[nodiscard]] virtual std::optional<std::vector<ResponseUser>>
  users() const = 0;
};
