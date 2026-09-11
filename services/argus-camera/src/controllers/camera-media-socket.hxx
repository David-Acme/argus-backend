#pragma once

#include "camera-media-service.hxx"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/WebSocketController.h>
#include <string>

// argus-camera media socket: camera:* control and fMP4 frames.
class CameraMediaSocket
    : public drogon::WebSocketController<CameraMediaSocket, false>
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
  WS_PATH_ADD("/media", "DeviceFilter", "JwtFilter");
  WS_PATH_LIST_END

private:
  CameraMediaService service_;
};
