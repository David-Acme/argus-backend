#pragma once

#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <feature/call/vocabulary/response-state.hxx>
#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct CallResponseSchema
{
  int64_t id{0};
  std::string dedupeKey;
  std::string kind;
  int64_t environmentId{0};
  int64_t episodeId{0};
  int64_t cameraId{0};
  std::string strategy;
  ResponseState state{ResponseState::Active};
  int step{0};
  int stepCount{1};
  int stepSeconds{45};
  int64_t stepDeadline{0};
  int64_t responderId{0};
  std::string responderName;
  std::optional<ResponseVerdict> verdict;
  int64_t verdictBy{0};
  std::string verdictByName;
  int64_t verdictAt{0};
  Json::Value plan;
  Json::Value data;
  int64_t createdAt{0};
  int64_t updatedAt{0};

  static CallResponseSchema fromRow(const drogon::orm::Row& row);
};

struct CallResponseMember
{
  int64_t responseId{0};
  int64_t userId{0};
  int step{0};
  ResponseMemberMode mode{ResponseMemberMode::Call};
  bool mandatory{false};
  bool discreet{false};
  int64_t reachedAt{0};

  static CallResponseMember fromRow(const drogon::orm::Row& row);
};

struct ResponsePlanEntry
{
  int64_t userId{0};
  int step{0};
  ResponseMemberMode mode{ResponseMemberMode::Call};
  bool mandatory{false};
  bool discreet{false};
};

struct ParsedResponsePlan
{
  std::string strategy;
  int stepCount{1};
  int stepSeconds{45};
  std::vector<ResponsePlanEntry> entries;
};

namespace call_response
{
inline constexpr int kMaxSteps = 33;
inline constexpr int kMinStepSeconds = 15;
inline constexpr int kMaxStepSeconds = 300;

std::optional<ParsedResponsePlan> parsePlan(const Json::Value& plan);

struct ViewInput
{
  const CallResponseSchema& response;
  const CallResponseMember* member{nullptr};
};

Json::Value toJson(const ViewInput& input);

std::string responseId(int64_t id);
}
