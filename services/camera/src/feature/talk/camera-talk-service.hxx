#pragma once

#include "camera-talk-session.hxx"

#include <drogon/WebSocketConnection.h>
#include <drogon/utils/coroutine.h>
#include <shared/repositories/camera/camera-repository.hxx>
#include <sync/sync-forwarder.hxx>

#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

struct TalkLimits
{
  int idleMs{20000};
  int maxMs{15 * 60 * 1000};
  size_t maxSessions{4};
};

class CameraTalkService
{
public:
  explicit CameraTalkService(TalkLimits limits = {}, std::function<bool()> active = {});
  ~CameraTalkService();

  CameraTalkService(const CameraTalkService&) = delete;
  CameraTalkService& operator=(const CameraTalkService&) = delete;

  [[nodiscard]] static bool handles(const std::string& type);

  drogon::Task<bool> handleText(const SyncFrameInput& input);
  void handleBinary(const drogon::WebSocketConnectionPtr& conn, std::span<const uint8_t> data);
  void handleClose(const drogon::WebSocketConnectionPtr& conn);

  size_t stopAll(const std::string& reason);
  void requestStop();
  [[nodiscard]] bool drained() const;
  [[nodiscard]] size_t active() const;

private:
  drogon::Task<void> start(const SyncFrameInput& input);
  void stopConnection(const drogon::WebSocketConnectionPtr& conn, const std::string& reason);
  void reap();

  TalkLimits limits_;
  std::function<bool()> active_;
  CameraRepository cameraRepository_;
  mutable std::mutex mutex_;
  std::unordered_map<const void*, std::shared_ptr<CameraTalkSession>> sessions_;
  std::vector<std::shared_ptr<CameraTalkSession>> retired_;
  bool stopping_{false};
};
