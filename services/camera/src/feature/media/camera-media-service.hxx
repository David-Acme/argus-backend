#pragma once

#include <algorithm>
#include <cstdint>
#include <drogon/WebSocketController.h>
#include <drogon/utils/coroutine.h>
#include <sync/sync-forwarder.hxx>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/services/stream/ws-frame.hxx>
#include <unordered_map>

class CameraStreamSink final : public StreamHub::ISink
{
public:
  CameraStreamSink(drogon::WebSocketConnectionPtr conn, int64_t window)
      : conn_(std::move(conn)), window_(window),
        maxPending_(std::max<int64_t>(window * kPendingWindows, kMinPendingBytes))
  {
  }

  static constexpr int64_t kPendingWindows = 16;
  static constexpr int64_t kMinPendingBytes = int64_t{8} * 1024 * 1024;

  bool sendBinary(const uint8_t* data, size_t len) override
  {
    if (!conn_ || conn_->disconnected())
      return false;
    {
      std::scoped_lock lock(mutex_);
      pending_ += static_cast<int64_t>(len - std::min(len, ws_frame::kHeaderSize));
      if (pending_ > maxPending_) {
        conn_->shutdown(drogon::CloseCode::kViolation, "slow_consumer");
        return false;
      }
    }
    conn_->send(reinterpret_cast<const char*>(data), len,
                drogon::WebSocketMessageType::Binary);
    return true;
  }

  bool tryReserve(size_t bytes) override
  {
    std::scoped_lock lock(mutex_);
    if (inFlight_ > 0 && inFlight_ + static_cast<int64_t>(bytes) > window_)
      return false;
    inFlight_ += static_cast<int64_t>(bytes);
    return true;
  }

  int64_t release(int64_t bytes) override
  {
    std::scoped_lock lock(mutex_);
    const int64_t accepted = std::clamp<int64_t>(bytes, 0, inFlight_);
    inFlight_ -= accepted;
    pending_ = std::max<int64_t>(0, pending_ - accepted);
    return accepted;
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

private:
  drogon::WebSocketConnectionPtr conn_;
  int64_t window_;
  int64_t maxPending_;
  int64_t inFlight_{0};
  int64_t pending_{0};
  mutable std::mutex mutex_;
};

class CameraMediaService
{
public:
  CameraMediaService();

  [[nodiscard]] static int64_t streamWindowBytes();

  drogon::Task<bool> handleText(const SyncFrameInput& input);
  void handleClose(const drogon::WebSocketConnectionPtr& conn);

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
