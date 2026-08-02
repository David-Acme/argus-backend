#pragma once

#include <drogon/WebSocketController.h>
#include <feature/socket/sync/services/sync-service.hxx>

class SyncSocket : public drogon::WebSocketController<SyncSocket>
{
public:
  void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                        std::string&& message,
                        const drogon::WebSocketMessageType& type) override;
  void handleNewConnection(const drogon::HttpRequestPtr& req,
                           const drogon::WebSocketConnectionPtr& conn) override;
  void handleConnectionClosed(
      const drogon::WebSocketConnectionPtr& conn) override;

  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/sync", "JwtFilter");
  WS_PATH_LIST_END

private:
  SyncService service_;
};
