#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/guard/dtos/update-response-dto.hxx>
#include <feature/guard/repositories/environment/environment-repository.hxx>
#include <feature/guard/repositories/response/response-repository.hxx>
#include <feature/guard/services/response-plan.hxx>
#include <functional>
#include <json/value.h>
#include <memory>

struct ResponseFeatureDependencies
{
  std::shared_ptr<const ResponseDirectory> directory;
  std::function<int64_t()> clock;
};

class ResponseFeatureService
{
public:
  explicit ResponseFeatureService(ResponseFeatureDependencies dependencies);

  struct ViewInput
  {
    int64_t environmentId{0};
    int64_t userId{0};
    UserRole role{UserRole::Guest};
  };

  [[nodiscard]] drogon::Task<Json::Value> view(const ViewInput& input) const;

  struct ReplaceInput
  {
    int64_t environmentId{0};
    int64_t userId{0};
    const UpdateResponseDto& body;
  };

  [[nodiscard]] drogon::Task<Json::Value>
  replace(const ReplaceInput& input) const;

  struct DutyInput
  {
    int64_t environmentId{0};
    int64_t userId{0};
    UserRole role{UserRole::Guest};
    bool onDuty{false};
  };

  [[nodiscard]] drogon::Task<Json::Value> setDuty(const DutyInput& input) const;

private:
  drogon::Task<std::vector<ResponseUser>> users() const;

  int64_t now() const;

  ResponseFeatureDependencies dependencies_;
  EnvironmentRepository environmentRepository_;
  ResponseRepository repository_;
};
