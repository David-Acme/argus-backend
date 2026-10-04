#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/call/dtos/update-call-preference-dto.hxx>
#include <feature/call/repositories/call-preference/call-preference-repository.hxx>

#include <cstdint>

class CallPreferenceService
{
public:
  CallPreferenceService() = default;

  drogon::Task<CallPreferenceSchema> read(int64_t userId) const;

  drogon::Task<CallPreferenceSchema>
  update(int64_t userId, const UpdateCallPreferenceDto& dto) const;

private:
  CallPreferenceRepository repository_;
};
