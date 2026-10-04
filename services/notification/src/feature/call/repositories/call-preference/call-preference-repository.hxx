#pragma once

#include "call-preference-query.hxx"

#include <drogon/utils/coroutine.h>
#include <feature/call/schemas/call-preference/call-preference-schema.hxx>

#include <optional>
#include <unordered_map>
#include <vector>

class CallPreferenceRepository
{
public:
  CallPreferenceRepository() = default;

  drogon::Task<std::optional<CallPreferenceSchema>> find(int64_t userId) const;

  drogon::Task<std::unordered_map<int64_t, CallPreferenceSchema>>
  findMany(const std::vector<int64_t>& userIds) const;

  drogon::Task<std::vector<CallPreferenceSchema>> findArrivalSubscribers() const;

  drogon::Task<CallPreferenceSchema>
  update(const CallPreferenceUpdateInput& input) const;
};
