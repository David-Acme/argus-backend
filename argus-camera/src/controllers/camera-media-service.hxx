#pragma once

#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <unordered_map>

// Binary sink of an fMP4 relay into one WebSocket connection, with the same
// credit window the legacy SyncMediaService uses. The class mirrors the
// legacy DrogonStreamSink byte for byte on the wire (camera:closed envelope,
// window semantics); it lives here because the legacy object drags the voice
// session and its AI stack with it.
class CameraStreamSink final : public StreamHub::ISink
{
public:
  CameraStreamSink(drogon::WebSocketConnectionPtr conn, int64_t window)
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
    subscriptions_ = std::max(0, subscriptions_ - 1);
  }

private:
  drogon::WebSocketConnectionPtr conn_;
  int64_t window_;
  int64_t inFlight_{0};
  int subscriptions_{0};
  mutable std::mutex mutex_;
};

// Native camera:* handling of argus-camera's /sync socket: the StreamHub
// lifecycle against its own go2rtc. Voice frames never reach this service;
// the gateway keeps them pointed at the legacy (talk stays TTS-load-bearing
// there until Fase 4).
class CameraMediaService : public SyncForwarder
{
public:
  CameraMediaService();

  void onConnect(const drogon::HttpRequestPtr&,
                 const drogon::WebSocketConnectionPtr&) override
  {
  }

  drogon::Task<bool> forwardText(const drogon::WebSocketConnectionPtr& conn,
                                 const Json::Value& message,
                                 std::string_view raw) override;
  void forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                     const std::string& data) override;
  void onClose(const drogon::WebSocketConnectionPtr& conn) override;

private:
  std::shared_ptr<CameraStreamSink>
  sinkFor(const drogon::WebSocketConnectionPtr& conn) const;
  void storeSink(const drogon::WebSocketConnectionPtr& conn,
                 const std::shared_ptr<CameraStreamSink>& sink) const;
  void dropSink(const drogon::WebSocketConnectionPtr& conn) const;

  CameraRepository cameraRepository_;
  mutable std::unordered_map<const void*, std::shared_ptr<CameraStreamSink>>
      sinks_;
  mutable std::mutex sinksMutex_;
  int maxSubsPerClient_{4};
};
