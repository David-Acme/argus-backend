#include "modules-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <errors/response-exception.hxx>
#include <feature/modules/dtos/impact-module-dto.hxx>
#include <feature/modules/dtos/response-list-modules-dto.hxx>
#include <feature/modules/dtos/response-module-data-dto.hxx>
#include <feature/modules/dtos/response-module-dto.hxx>
#include <feature/modules/dtos/response-module-impact-dto.hxx>
#include <feature/modules/dtos/response-module-request-dto.hxx>
#include <feature/modules/dtos/uninstall-module-dto.hxx>
#include <feature/modules/module-errors.hxx>
#include <http/api-response.hxx>

#include <utility>

namespace
{
ModuleCommand commandOf(const drogon::HttpRequestPtr& req, std::string id)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  return {.moduleId = std::move(id), .userId = jwt.sub};
}
}

ModulesController::ModulesController(ModuleEngine* engine) : engine_(engine) {}

ModuleEngine& ModulesController::engine() const
{
  if (engine_ == nullptr)
    throw ResponseException(ModuleErrors::Unavailable);
  return *engine_;
}

std::string_view ModulesController::languageOf(const drogon::HttpRequestPtr& req)
{
  const auto& header = req->getHeader("accept-language");
  return header.size() >= 2 && (header.starts_with("en") || header.starts_with("EN")) ? "en" : "es";
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::list(drogon::HttpRequestPtr req)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  auto views = co_await engine().listAsync();
  co_return ApiResponse::ok(ResponseListModulesDto{
      .modules = std::move(views), .owner = jwt.role == UserRole::Owner, .lang = std::string(languageOf(req))}
                                .toJson());
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::install(drogon::HttpRequestPtr req, std::string id)
{
  const auto job = co_await engine().installAsync(commandOf(req, std::move(id)));
  co_return ApiResponse::accepted(ResponseModuleJobDto{.job = job}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::pause(drogon::HttpRequestPtr req, std::string id)
{
  const auto job = co_await engine().pauseAsync(commandOf(req, std::move(id)));
  co_return ApiResponse::ok(ResponseModuleJobDto{.job = job}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::resume(drogon::HttpRequestPtr req, std::string id)
{
  const auto job = co_await engine().resumeAsync(commandOf(req, std::move(id)));
  co_return ApiResponse::ok(ResponseModuleJobDto{.job = job}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::cancel(drogon::HttpRequestPtr req, std::string id)
{
  const auto job = co_await engine().cancelAsync(commandOf(req, std::move(id)));
  co_return ApiResponse::ok(ResponseModuleJobDto{.job = job}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::disable(drogon::HttpRequestPtr req, std::string id)
{
  std::string lang(languageOf(req));
  auto module = co_await engine().disableAsync(commandOf(req, std::move(id)));
  co_return ApiResponse::ok(ResponseModuleDto{.module = std::move(module), .lang = std::move(lang)}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::uninstall(drogon::HttpRequestPtr req, std::string id)
{
  auto body = UninstallModuleDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto job = co_await engine().uninstallAsync({.moduleId = std::move(id),
                                                     .userId = jwt.sub,
                                                     .keepData = body.keepData,
                                                     .pin = std::move(body.pin),
                                                     .reassign = std::move(body.reassign)});
  co_return ApiResponse::accepted(ResponseModuleJobDto{.job = job}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::data(drogon::HttpRequestPtr, std::string id)
{
  auto owners = co_await engine().moduleDataAsync(std::move(id));
  co_return ApiResponse::ok(ResponseModuleDataDto{.owners = std::move(owners)}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::impact(drogon::HttpRequestPtr req, std::string id)
{
  const auto query = ImpactModuleDto::fromAction(req->getParameter("action"));
  auto view = co_await engine().impactAsync({.moduleId = std::move(id), .action = query.impactAction});
  co_return ApiResponse::ok(ResponseModuleImpactDto{.impact = std::move(view)}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ModulesController::requestModule(drogon::HttpRequestPtr req, std::string id)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  auto view = co_await engine().requestAsync({.moduleId = std::move(id), .userId = jwt.sub, .role = jwt.role});
  co_return ApiResponse::ok(ResponseModuleRequestDto{.request = std::move(view)}.toJson());
}
