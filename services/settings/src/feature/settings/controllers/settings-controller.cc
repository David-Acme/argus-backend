#include "settings-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <feature/settings/dtos/response-list-settings-dto.hxx>
#include <feature/settings/dtos/response-update-settings-dto.hxx>
#include <feature/settings/dtos/update-settings-dto.hxx>
#include <http/api-response.hxx>

#include <utility>

SettingsController::SettingsController(const SettingsGatewayInput& input) : service_(input) {}

drogon::Task<drogon::HttpResponsePtr> SettingsController::list(drogon::HttpRequestPtr)
{
  const auto owners = co_await service_.catalogsAsync();
  co_return ApiResponse::ok(ResponseListSettingsDto{.owners = owners}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> SettingsController::update(drogon::HttpRequestPtr req, std::string owner)
{
  auto body = UpdateSettingsDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto outcome = co_await service_.updateAsync(
      {.owner = std::move(owner), .changes = std::move(body.changes), .userId = jwt.sub});
  co_return ApiResponse::ok(ResponseUpdateSettingsDto{.outcome = outcome}.toJson());
}
