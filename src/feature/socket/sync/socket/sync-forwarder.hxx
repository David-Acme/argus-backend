#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <memory>
#include <string>
#include <string_view>

// Side channel of SyncService for the message types it does not serve from
// the sync tables itself (camera:* and voice:* frames plus raw binary).
// The backend installs the native media handler; the gateway installs the
// legacy relay. Unset, the non-sync types fall through to the standard
// unknown-type error.
class SyncForwarder
{
public:
  virtual ~SyncForwarder() = default;

  // Client connection established; the implementation may capture the
  // transport identity (token, user agent) for its own channel.
  virtual void onConnect(const drogon::HttpRequestPtr&,
                         const drogon::WebSocketConnectionPtr&)
  {
  }

  // One text frame of a relayed type. False means the type is not handled
  // and the caller answers with the standard unknown-type error.
  virtual drogon::Task<bool>
  forwardText(const drogon::WebSocketConnectionPtr& conn,
              const Json::Value& message, std::string_view raw) = 0;

  virtual void forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                             const std::string& data) = 0;

  virtual void onClose(const drogon::WebSocketConnectionPtr& conn) = 0;
};