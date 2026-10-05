#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <json/value.h>
#include <mutex>
#include <optional>
#include <string>
#include <trantor/net/EventLoop.h>

struct FrameLaneConfig
{
  double burst{120.0};
  double refillPerSecond{20.0};
  std::size_t maxQueued{64};
};

enum class FrameJobKind : uint8_t
{
  Frame,
  Revalidate
};

struct FrameJob
{
  FrameJobKind kind{FrameJobKind::Frame};
  Json::Value message;
  std::string raw;
  std::string type;
};

enum class FrameAdmission : uint8_t
{
  Start,
  Queued,
  Refused,
  Stopping
};

struct FrameLaneOwner
{
  int64_t userId{0};
  trantor::EventLoop* loop{nullptr};
};

class FrameLane
{
public:
  FrameLane(FrameLaneConfig config, FrameLaneOwner owner);

  [[nodiscard]] FrameAdmission admit(FrameJob job, double nowSeconds);
  [[nodiscard]] FrameAdmission admitRevalidation();
  [[nodiscard]] std::optional<FrameJob> next();

  void close();

  [[nodiscard]] bool draining() const;
  [[nodiscard]] int64_t userId() const { return owner_.userId; }
  [[nodiscard]] trantor::EventLoop* loop() const { return owner_.loop; }

private:
  [[nodiscard]] FrameAdmission enqueueLocked(FrameJob job);

  const FrameLaneConfig config_;
  const FrameLaneOwner owner_;
  mutable std::mutex mutex_;
  std::deque<FrameJob> queue_;
  double tokens_{0.0};
  std::optional<double> refilledAt_;
  bool draining_{false};
  bool revalidationQueued_{false};
  bool closed_{false};
};
