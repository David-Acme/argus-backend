#pragma once

#include <drogon/WebSocketClient.h>
#include <drogon/WebSocketController.h>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

// Internal camera media socket; empty disables the /camera-stream endpoint.
struct CameraStreamConfig
{
  std::string streamUrl;

  static CameraStreamConfig resolve();
};

// Camera media frames are the only ones this socket carries.
bool isCameraStreamFrame(std::string_view type);

// Byte-transparent relay from the /camera-stream socket to argus-camera,
// carrying the client's own credentials and transport identity.
class CameraStreamRelay
{
public:
  explicit CameraStreamRelay(std::string streamUrl);

  void onConnect(const drogon::HttpRequestPtr& req,
                 const drogon::WebSocketConnectionPtr& conn);
  drogon::Task<bool> forwardText(const SyncFrameInput& input);
  void onClose(const drogon::WebSocketConnectionPtr& conn);

private:
  struct Session;

  std::shared_ptr<Session> sessionFor(const drogon::WebSocketConnectionPtr& conn);
  std::shared_ptr<Session> takeSession(const drogon::WebSocketConnectionPtr& conn);
  drogon::Task<void> openSession(const drogon::WebSocketConnectionPtr& conn,
                                 std::shared_ptr<Session> session);

  const std::string streamUrl_;
  mutable std::mutex sessionsMutex_;
  std::unordered_map<const void*, std::shared_ptr<Session>> sessions_;
};
