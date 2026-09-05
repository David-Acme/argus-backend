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

struct LegacySyncConfig
{
  // Internal /sync of the legacy backend; empty disables the relay.
  std::string syncUrl;
  std::string dbPath;

  static LegacySyncConfig resolve();
};

// Every legacy-emitted text frame the client must still receive; module and
// sync emits of the relay session are dropped (the gateway serves those
// natively).
bool relayAllowedText(std::string_view type);

// Per-client byte-transparent relay to the legacy /sync socket. Each session
// connects with the client's own credentials (token and User-Agent; the
// X-Forwarded-For is synthesized from the observed TCP peer address) and is
// only touched from the event loop of its client connection. Binary frames
// past the pending-frame cap are dropped while the legacy session connects.
class LegacySyncRelay final : public SyncForwarder
{
public:
  explicit LegacySyncRelay(LegacySyncConfig config);

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

  const LegacySyncConfig config_;
  mutable std::mutex sessionsMutex_;
  std::unordered_map<const void*, std::shared_ptr<Session>> sessions_;
};
