#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/conversation/services/conversation-inbox-service.hxx>

#include <cstdint>
#include <memory>

class ConversationController : public drogon::HttpController<ConversationController, false>
{
public:
  explicit ConversationController(std::shared_ptr<const ConversationInboxService> inbox);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ConversationController::list, "/conversation", drogon::Get, "DeviceFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(ConversationController::markRead, "/conversation/{1}/read", drogon::Patch, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ConversationController::remove, "/conversation/{1}", drogon::Delete, "DeviceFilter", "JwtFilter",
                "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);

  drogon::Task<drogon::HttpResponsePtr> markRead(drogon::HttpRequestPtr req, int64_t counterpartId);

  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req, int64_t counterpartId);

private:
  std::shared_ptr<const ConversationInboxService> service_;
};
