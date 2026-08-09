#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <feature/socket/sync/services/synchronized-service.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <shared/services/room/room-manager.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <unordered_map>

class DrogonStreamSink final : public StreamHub::ISink
{
public:
  DrogonStreamSink(drogon::WebSocketConnectionPtr conn, int64_t window)
      : conn_(std::move(conn)), window_(window)
  {
  }

  bool sendBinary(const uint8_t* data, size_t len) override
  {
    if (!conn_ || conn_->disconnected())
      return false;
    conn_->send(reinterpret_cast<const char*>(data), len,
                drogon::WebSocketMessageType::Binary);
    return true;
  }

  bool tryReserve(size_t bytes) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inFlight_ + static_cast<int64_t>(bytes) > window_)
      return false;
    inFlight_ += static_cast<int64_t>(bytes);
    return true;
  }

  void release(int64_t bytes) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    inFlight_ = std::max<int64_t>(0, inFlight_ - bytes);
  }

  void onClosed(const StreamHub::StreamClosedInput& input) override
  {
    if (!conn_ || conn_->disconnected())
      return;
    Json::Value j;
    j["type"] = "camera:closed";
    j["payload"]["subId"] = input.subId;
    j["payload"]["reason"] = input.reason;
    conn_->sendJson(j);
  }

  int subscriptions() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_;
  }

  void addSubscription()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++subscriptions_;
  }

  void dropSubscription()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    --subscriptions_;
  }

private:
  drogon::WebSocketConnectionPtr conn_;
  mutable std::mutex mutex_;
  int64_t window_;
  int64_t inFlight_{0};
  int subscriptions_{0};
};

class SyncService
{
public:
  SyncService();

  drogon::Task<void>
  handleConnect(const drogon::HttpRequestPtr& req,
                const drogon::WebSocketConnectionPtr& conn) const;
  drogon::Task<void> handleMessage(const drogon::WebSocketConnectionPtr& conn,
                                   const Json::Value& obj) const;
  void handleDisconnect(const drogon::WebSocketConnectionPtr& conn) const;

private:
  std::shared_ptr<DrogonStreamSink>
  sinkFor(const drogon::WebSocketConnectionPtr& conn) const;
  void storeSink(const drogon::WebSocketConnectionPtr& conn,
                 const std::shared_ptr<DrogonStreamSink>& sink) const;
  void dropSink(const drogon::WebSocketConnectionPtr& conn) const;

  SynchronizedService synchronizedService_;
  RoomManager roomManager_;
  int maxSubsPerClient_{8};
  mutable std::mutex sinksMutex_;
  mutable std::unordered_map<const void*, std::shared_ptr<DrogonStreamSink>>
      sinks_;
};
