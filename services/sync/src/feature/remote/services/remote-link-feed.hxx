#pragma once

#include <feature/remote/services/remote-link-gate.hxx>

#include <trantor/net/EventLoop.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class NatsBus;

struct RemoteLinkFeedDependencies
{
  NatsBus* bus{nullptr};
};

struct RemoteLinkFeedConfig
{
  std::string subject;
  RemoteLinkGateConfig gate;
  double tickSeconds{1.0};
  int64_t refusalLogWindowMs{60'000};
};

class RemoteLinkFeed
{
public:
  RemoteLinkFeed(RemoteLinkFeedDependencies dependencies, RemoteLinkFeedConfig config);
  ~RemoteLinkFeed();
  RemoteLinkFeed(const RemoteLinkFeed&) = delete;
  RemoteLinkFeed& operator=(const RemoteLinkFeed&) = delete;

  void start();
  void stop();
  void requestStop();
  [[nodiscard]] bool drained() const;

  void ingest(std::string_view payload, int64_t nowMs);
  void tick(int64_t nowMs);

private:
  void publishState(int64_t nowMs);

  RemoteLinkFeedDependencies dependencies_;
  RemoteLinkFeedConfig config_;
  RemoteLinkGate gate_;
  RefusalLog refusals_;
  std::optional<uint64_t> subscription_;
  std::optional<trantor::TimerId> timer_;
  std::shared_ptr<bool> alive_;
  std::atomic<bool> stopped_{false};
};
