#pragma once

#include "camera-media-service.hxx"
#include "media-session-registry.hxx"

#include <feature/talk/camera-talk-service.hxx>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/WebSocketController.h>
#include <string>

class CameraMediaSocket
    : public drogon::WebSocketController<CameraMediaSocket, false>
{
public:
  CameraMediaSocket(MediaSessionRegistry& sessions, CameraTalkService& talk)
      : sessions_(sessions), talk_(talk)
  {
  }

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
  MediaSessionRegistry& sessions_;
  CameraTalkService& talk_;
};
