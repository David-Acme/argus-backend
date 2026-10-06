#pragma once

#include "call-response-query.hxx"

#include <drogon/utils/coroutine.h>
#include <feature/call/schemas/call-response/call-response-schema.hxx>

#include <cstdint>
#include <optional>
#include <vector>

struct CallResponseOpened
{
  std::optional<CallResponseSchema> response;
  bool created{false};
};

struct CallResponseForUser
{
  CallResponseSchema response;
  CallResponseMember member;
};

class CallResponseRepository
{
public:
  [[nodiscard]] drogon::Task<CallResponseOpened>
  open(const CallResponseCreateInput& input,
       const std::vector<CallResponseMemberInput>& members) const;

  [[nodiscard]] drogon::Task<std::optional<CallResponseSchema>> findById(int64_t id) const;

  [[nodiscard]] drogon::Task<std::optional<CallResponseSchema>>
  findByKey(const std::string& dedupeKey) const;

  [[nodiscard]] drogon::Task<std::vector<CallResponseMember>> members(int64_t responseId) const;

  [[nodiscard]] drogon::Task<std::optional<CallResponseMember>>
  member(int64_t responseId, int64_t userId) const;

  [[nodiscard]] drogon::Task<bool> markReached(const CallResponseReachInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<CallResponseSchema>>
  due(const CallResponseDueInput& input) const;

  [[nodiscard]] drogon::Task<bool> advance(const CallResponseAdvanceInput& input) const;

  [[nodiscard]] drogon::Task<bool> setDeadline(const CallResponseAdvanceInput& input) const;

  [[nodiscard]] drogon::Task<bool> markUnanswered(int64_t id, int64_t at) const;

  [[nodiscard]] drogon::Task<bool> attend(const CallResponseAttendInput& input) const;

  [[nodiscard]] drogon::Task<bool> verdict(const CallResponseVerdictInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<int64_t>> expire(int64_t createdBefore, int64_t at) const;
  [[nodiscard]] drogon::Task<std::vector<int64_t>> expireKinds(const CallResponseExpireKindsInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<CallResponseForUser>>
  forUser(const CallResponseUserInput& input) const;

  [[nodiscard]] drogon::Task<int64_t> purgeClosed(int64_t updatedBefore) const;
};
