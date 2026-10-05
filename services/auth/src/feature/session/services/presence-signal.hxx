#pragma once

#include <auth/session-origin.hxx>
#include <auth/session-platform.hxx>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

struct PresenceSignal
{
  int64_t userId{0};
  std::string sessionId;
  SessionPlatform platform{SessionPlatform::Unknown};
  SessionOrigin origin{SessionOrigin::Unknown};
  int64_t at{0};
};

class PresenceSignalSink
{
public:
  PresenceSignalSink() = default;
  PresenceSignalSink(const PresenceSignalSink&) = delete;
  PresenceSignalSink& operator=(const PresenceSignalSink&) = delete;
  virtual ~PresenceSignalSink() = default;

  virtual void publish(const PresenceSignal& signal) = 0;
};

struct PresenceThrottleInput
{
  std::string_view sessionId;
  SessionOrigin origin{SessionOrigin::Unknown};
  bool seenAdvanced{false};
};

class PresenceSignalThrottle
{
public:
  static constexpr std::size_t kMaxSessions = 4096;

  [[nodiscard]] static bool informative(SessionOrigin origin);

  [[nodiscard]] bool admit(const PresenceThrottleInput& input);

private:
  std::mutex mutex_;
  std::unordered_map<std::string, SessionOrigin> lastOrigin_;
};
