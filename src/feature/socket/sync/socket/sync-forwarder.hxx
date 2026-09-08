#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <memory>
#include <string>
#include <string_view>

// Side channel of SyncService for the frame types the sync tables do not serve themselves.
class SyncForwarder
{
public:
  virtual ~SyncForwarder() = default;

  // Client connection established; the implementation may capture the transport identity.
  virtual void onConnect(const drogon::HttpRequestPtr&,
                         const drogon::WebSocketConnectionPtr&)
  {
  }

  // One text frame of a relayed type; false means it was not handled.
  virtual drogon::Task<bool>
  forwardText(const drogon::WebSocketConnectionPtr& conn,
              const Json::Value& message, std::string_view raw) = 0;

  virtual void forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                             const std::string& data) = 0;

  virtual void onClose(const drogon::WebSocketConnectionPtr& conn) = 0;
};
