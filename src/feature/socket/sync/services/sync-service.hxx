#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <feature/socket/sync/services/synchronized-service.hxx>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <json/value.h>
#include <memory>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/room/room-manager.hxx>
#include <string_view>

// Serves the sync tables natively (sync, sync_audit_log,
// sync_user_audit_log) and hands every other frame plus raw binary to the
// installed forwarder (native media service on the backend, legacy relay on
// the gateway).
class SyncService
{
public:
  drogon::Task<void>
  handleConnect(const drogon::HttpRequestPtr& req,
                const drogon::WebSocketConnectionPtr& conn) const;
  drogon::Task<void> handleMessage(const drogon::WebSocketConnectionPtr& conn,
                                   const Json::Value& obj,
                                   std::string_view rawMessage) const;
  void handleBinary(const drogon::WebSocketConnectionPtr& conn,
                    const std::string& data) const;
  void handleDisconnect(const drogon::WebSocketConnectionPtr& conn) const;

  void setForwarder(std::shared_ptr<SyncForwarder> forwarder);

private:
  drogon::Task<void>
  refreshContext(const drogon::WebSocketConnectionPtr& conn) const;

  SynchronizedService synchronizedService_;
  RoomManager roomManager_;
  UserRepository userRepository_;
  std::shared_ptr<SyncForwarder> forwarder_;
};
