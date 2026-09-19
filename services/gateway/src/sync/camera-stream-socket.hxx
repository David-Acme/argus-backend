#pragma once

#include "camera-stream-relay.hxx"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/WebSocketController.h>
#include <memory>
#include <string>

// Client-facing camera media socket; only camera frames reach the relay.
class CameraStreamSocket
    : public drogon::WebSocketController<CameraStreamSocket, false>
{
public:
  void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                        std::string&& message,
                        const drogon::WebSocketMessageType& type) override;
  void handleNewConnection(const drogon::HttpRequestPtr& req,
                           const drogon::WebSocketConnectionPtr& conn) override;
  void handleConnectionClosed(
      const drogon::WebSocketConnectionPtr& conn) override;

  void setRelay(std::shared_ptr<CameraStreamRelay> relay);

  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/camera-stream", "DeviceFilter", "JwtFilter");
  WS_PATH_LIST_END

private:
  std::shared_ptr<CameraStreamRelay> relay_;
};
