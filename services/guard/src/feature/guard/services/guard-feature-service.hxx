#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/guard/dtos/create-expected-guest-dto.hxx>
#include <feature/guard/dtos/list-decisions-dto.hxx>
#include <feature/guard/guard-repository.hxx>
#include <feature/guard/guard-schedule.hxx>
#include <json/value.h>
#include <string>

class IdentityClient;

struct GuardFeatureDependencies
{
  IdentityClient* identity{nullptr};
  GuardMode defaultMode{GuardMode::Home};
  GuardScheduleConfig schedule;
};

class GuardFeatureService
{
public:
  struct PromotePersonInput
  {
    int64_t personId{0};
    std::string accessToken;
    std::string deviceHash;
  };

  explicit GuardFeatureService(const GuardFeatureDependencies& dependencies);

  drogon::Task<bool> promotePerson(const PromotePersonInput& input) const;

  drogon::Task<Json::Value> mode() const;

  drogon::Task<std::string> setMode(const std::string& mode) const;

  drogon::Task<Json::Value> incidents(int limit) const;

  drogon::Task<Json::Value> decisions(const ListDecisionsDto& query) const;

  drogon::Task<Json::Value>
  decisionsSummary(const DecisionsSummaryInput& input) const;

  drogon::Task<bool> setFeedback(const std::string& eventId,
                                 const std::string& label) const;

  drogon::Task<int64_t> createGuest(
      const CreateExpectedGuestDto& input) const;

  drogon::Task<Json::Value> guests() const;

  drogon::Task<bool> removeGuest(int64_t id) const;

private:
  IdentityClient* identity_{nullptr};
  GuardMode defaultMode_{GuardMode::Home};
  GuardSchedule schedule_;
  GuardRepository repository_;
};
