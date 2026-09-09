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
#include <vector>

// Internal /sync of argus-camera; empty disables the camera relay leg.
struct CameraSyncConfig
{
  std::string syncUrl;

  static CameraSyncConfig resolve();
};

// Relay-leg text frames the client must still receive; the gateway serves module emits.
bool relayAllowedText(std::string_view type);

// camera:* frames belong to the argus-camera leg, the rest to the voice leg.
bool relayLegIsCamera(std::string_view type);

// voice:* frames and raw binary belong to the argus-voice leg.
bool relayLegIsVoice(std::string_view type);

// Byte-transparent relay to a service /sync leg with the client's own credentials.
class LegacySyncRelay final : public SyncForwarder
{
public:
  explicit LegacySyncRelay(std::string syncUrl);

  void onConnect(const drogon::HttpRequestPtr& req,
                 const drogon::WebSocketConnectionPtr& conn) override;
  drogon::Task<bool> forwardText(const drogon::WebSocketConnectionPtr& conn,
                                 const Json::Value& message,
                                 std::string_view raw) override;
  void forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                     const std::string& data) override;
  void onClose(const drogon::WebSocketConnectionPtr& conn) override;

private:
  struct Session;
  struct Frame
  {
    std::string data;
    bool binary{false};
  };

  std::shared_ptr<Session> sessionFor(const drogon::WebSocketConnectionPtr& conn);
  std::shared_ptr<Session> takeSession(const drogon::WebSocketConnectionPtr& conn);
  drogon::Task<void> openSession(const drogon::WebSocketConnectionPtr& conn,
                                 std::shared_ptr<Session> session);

  const std::string syncUrl_;
  mutable std::mutex sessionsMutex_;
  std::unordered_map<const void*, std::shared_ptr<Session>> sessions_;
};

// Relay split: camera:* to argus-camera, voice:* and binary to argus-voice.
class CompositeSyncRelay final : public SyncForwarder
{
public:
  CompositeSyncRelay(std::shared_ptr<SyncForwarder> cameraLeg,
                     std::shared_ptr<SyncForwarder> voiceLeg);

  void onConnect(const drogon::HttpRequestPtr& req,
                 const drogon::WebSocketConnectionPtr& conn) override;
  drogon::Task<bool> forwardText(const drogon::WebSocketConnectionPtr& conn,
                                 const Json::Value& message,
                                 std::string_view raw) override;
  void forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                     const std::string& data) override;
  void onClose(const drogon::WebSocketConnectionPtr& conn) override;

private:
  std::shared_ptr<SyncForwarder> cameraLeg_;
  std::shared_ptr<SyncForwarder> voiceLeg_;
};
