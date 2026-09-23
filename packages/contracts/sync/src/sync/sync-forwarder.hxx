#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <memory>
#include <string>
#include <string_view>

struct SyncFrameInput
{
  const drogon::WebSocketConnectionPtr& conn;
  const Json::Value& message;
  std::string_view raw;
};

struct SocketFrameError
{
  const drogon::WebSocketConnectionPtr& conn;
  const std::string& type;
  int status{500};
  const std::string& error;
};

inline void sendSocketFrameError(const SocketFrameError& input)
{
  Json::Value envelope;
  envelope["type"] = input.type + "_error";
  envelope["status"] = input.status;
  envelope["error"] = input.error;
  input.conn->sendJson(envelope);
}

class SyncForwarder
{
public:
  virtual ~SyncForwarder() = default;

  virtual void onConnect(const drogon::HttpRequestPtr&,
                         const drogon::WebSocketConnectionPtr&)
  {
  }

  virtual drogon::Task<bool> forwardText(const SyncFrameInput& input) = 0;

  virtual void forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                             const std::string& data) = 0;

  virtual void onClose(const drogon::WebSocketConnectionPtr& conn) = 0;
};
