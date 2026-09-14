#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/guard/dtos/create-expected-guest-dto.hxx>
#include <guard-repository.hxx>
#include <json/value.h>
#include <string>

class IdentityClient;

// Guard feature gateway: mode, incident reads, expected-guest windows and the
// owner-token promotion forwarder.
class GuardFeatureService
{
public:
  struct PromotePersonInput
  {
    int64_t personId{0};
    std::string accessToken;
    std::string deviceHash;
  };

  explicit GuardFeatureService(IdentityClient* identity);

  drogon::Task<bool> promotePerson(const PromotePersonInput& input) const;

  drogon::Task<std::string> mode() const;

  drogon::Task<std::string> setMode(const std::string& mode) const;

  drogon::Task<Json::Value> incidents(int limit) const;

  drogon::Task<int64_t> createGuest(
      const CreateExpectedGuestDto& input) const;

  drogon::Task<Json::Value> guests() const;

  drogon::Task<bool> removeGuest(int64_t id) const;

private:
  IdentityClient* identity_{nullptr};
  GuardRepository repository_;
};
