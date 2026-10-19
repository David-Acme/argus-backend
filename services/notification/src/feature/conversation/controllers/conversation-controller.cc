#include "conversation-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <errors/response-exception.hxx>
#include <feature/conversation/dtos/conversation-thread-dto.hxx>
#include <feature/conversation/dtos/response-conversation-threads-dto.hxx>
#include <http/api-response.hxx>
#include <notification/notification-errors.hxx>

#include <utility>

ConversationController::ConversationController(std::shared_ptr<const ConversationInboxService> inbox)
    : service_(std::move(inbox))
{
}

drogon::Task<drogon::HttpResponsePtr> ConversationController::list(drogon::HttpRequestPtr req)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const ResponseConversationThreadsDto result{.threads = co_await service_->threads(ctx.sub)};
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ConversationController::markRead(drogon::HttpRequestPtr req,
                                                                       int64_t counterpartId)
{
  const auto thread = ConversationThreadDto::fromPath(counterpartId);
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  Json::Value result(Json::objectValue);
  result["updated"] = static_cast<Json::Int64>(
      co_await service_->markThreadRead({.userId = ctx.sub, .counterpartId = thread.counterpartId}));
  co_return ApiResponse::ok(result);
}

drogon::Task<drogon::HttpResponsePtr> ConversationController::remove(drogon::HttpRequestPtr req, int64_t counterpartId)
{
  const auto thread = ConversationThreadDto::fromPath(counterpartId);
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  if (!co_await service_->removeThread({.userId = ctx.sub, .counterpartId = thread.counterpartId}))
    throw ResponseException(NotificationErrors::ConversationNotFound);
  co_return ApiResponse::noContent();
}
