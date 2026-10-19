#include "rtc-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/linked-filter.hxx>
#include <auth/request-context.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <errors/response-exception.hxx>
#include <feature/rtc/dtos/rtc-decline-dto.hxx>
#include <http/api-response.hxx>

#include <chrono>
#include <exception>

#include <utility>

RtcController::RtcController(RtcTokenServiceInput input) : service_(std::move(input))
{
  auth_filters::requireLinked<DeviceFilter>();
  auth_filters::requireLinked<ValidJsonFilter>();
  auth_filters::requireLinked<JwtFilter>();
  auth_filters::requireLinked<RoleFilter>();
}

drogon::Task<drogon::HttpResponsePtr> RtcController::token(drogon::HttpRequestPtr req)
{
  const auto body = RtcTokenDto::fromJson(*req->getJsonObject());
  const auto& caller = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  try {
    const auto result = co_await service_.issue(
        {.body = body, .caller = caller, .host = req->getHeader("host")});
    LOG_INFO << "RTC token issued: user " << caller.sub << " call " << body.callId;
    co_return ApiResponse::ok(result.toJson());
  }
  catch (const ResponseException& error) {
    LOG_INFO << "RTC token refused: user " << caller.sub << " call " << body.callId << " status "
             << error.statusCode() << " reason " << error.errorCode();
    throw;
  }
  catch (const std::exception& error) {
    LOG_WARN << "RTC token failed: user " << caller.sub << " call " << body.callId << " status 500 "
             << error.what();
    throw;
  }
}

drogon::Task<drogon::HttpResponsePtr> RtcController::decline(drogon::HttpRequestPtr req)
{
  const auto body = RtcDeclineDto::fromJson(*req->getJsonObject());
  const auto& caller = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await service_.decline({.body = body, .caller = caller});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr> RtcController::ice(drogon::HttpRequestPtr req)
{
  const auto& caller = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  co_return ApiResponse::ok(service_.ice({.caller = caller, .now = now}).toJson());
}

drogon::Task<drogon::HttpResponsePtr> RtcController::watch(drogon::HttpRequestPtr req)
{
  const auto& caller = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await service_.watchConversations(caller);
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr> RtcController::endWatch(drogon::HttpRequestPtr req, std::string conversationId)
{
  const auto body = RtcWatchEndDto::fromPath(conversationId);
  const auto& caller = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await service_.endWatch({.body = body, .caller = caller});
  co_return ApiResponse::ok(result.toJson());
}
