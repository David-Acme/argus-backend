#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <feature/socket/sync/services/voice-session-service.hxx>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <json/value.h>

// Voice handler of the voice:* frames: one voice session per connection.
// The camera:* frames belong to argus-camera's CameraMediaService; the
// gateway relays them there.
class SyncMediaService final : public SyncForwarder
{
public:
  drogon::Task<bool> forwardText(const drogon::WebSocketConnectionPtr& conn,
                                 const Json::Value& message,
                                 std::string_view raw) override;
  void forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                     const std::string& data) override;
  void onClose(const drogon::WebSocketConnectionPtr& conn) override;

private:
  VoiceSessionService voiceSessionService_;
  UserRepository userRepository_;
};
