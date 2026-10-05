#pragma once

#include <auth/user-directory.hxx>
#include <drogon/WebSocketConnection.h>
#include <drogon/utils/coroutine.h>
#include <feature/transport/services/frame-lane.hxx>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <trantor/net/EventLoop.h>
#include <unordered_map>
#include <vector>

struct LaneOpenInput
{
  const drogon::WebSocketConnectionPtr& conn;
  int64_t userId{0};
  trantor::EventLoop* loop{nullptr};
};

struct LaneRevalidationConfig
{
  double intervalSeconds{60.0};
  std::shared_ptr<const IUserDirectory> directory;
};

class ConnectionLanes
{
public:
  using DrainStarter = std::function<void(const drogon::WebSocketConnectionPtr&,
                                          const std::shared_ptr<FrameLane>&)>;

  explicit ConnectionLanes(FrameLaneConfig config = {});
  ~ConnectionLanes();

  ConnectionLanes(const ConnectionLanes&) = delete;
  ConnectionLanes& operator=(const ConnectionLanes&) = delete;
  ConnectionLanes(ConnectionLanes&&) = delete;
  ConnectionLanes& operator=(ConnectionLanes&&) = delete;

  void setDrainStarter(DrainStarter starter);

  std::shared_ptr<FrameLane> open(const LaneOpenInput& input);
  [[nodiscard]] std::shared_ptr<FrameLane>
  find(const drogon::WebSocketConnectionPtr& conn) const;
  void close(const drogon::WebSocketConnectionPtr& conn);

  std::size_t revalidateUser(int64_t userId);
  [[nodiscard]] std::vector<int64_t> connectedUsers() const;

  void startRevalidation(LaneRevalidationConfig config);
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  struct Entry
  {
    std::weak_ptr<drogon::WebSocketConnection> conn;
    std::shared_ptr<FrameLane> lane;
  };

  drogon::Task<void> revalidateAll();
  void pruneExpired();
  void startDrain(const drogon::WebSocketConnectionPtr& conn,
                  const std::shared_ptr<FrameLane>& lane) const;

  const FrameLaneConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<const drogon::WebSocketConnection*, Entry> entries_;
  DrainStarter starter_;
  std::shared_ptr<const IUserDirectory> directory_;
  std::optional<trantor::TimerId> timer_;
  std::atomic<bool> sweeping_{false};
  std::atomic<bool> stopping_{false};
};
