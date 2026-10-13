#pragma once

#include <routes/remote-link.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct RemoteLinkGateConfig
{
  int64_t staleAfterMs{90'000};
  int64_t throttleMs{5'000};
};

class RemoteLinkGate
{
public:
  explicit RemoteLinkGate(RemoteLinkGateConfig config);

  routes::RemoteLinkRefusal ingest(std::string_view payload, int64_t nowMs);
  bool expire(int64_t nowMs);
  [[nodiscard]] bool shouldSend(int64_t nowMs) const;
  void markSent(int64_t nowMs);

  [[nodiscard]] const std::optional<routes::RemoteLinkState>& state() const { return state_; }

private:
  struct Visible
  {
    std::string homeId;
    std::string domain;
    routes::RemoteLink link{routes::RemoteLink::Down};

    bool operator==(const Visible&) const = default;
  };

  [[nodiscard]] std::optional<Visible> visible() const;

  RemoteLinkGateConfig config_;
  std::optional<routes::RemoteLinkState> state_;
  int64_t lastHeardAtMs_{0};
  std::optional<Visible> lastSent_;
  std::optional<int64_t> lastSentAtMs_;
};

class RefusalLog
{
public:
  explicit RefusalLog(int64_t windowMs);

  [[nodiscard]] std::optional<uint64_t> record(int64_t nowMs);

private:
  int64_t windowMs_;
  std::optional<int64_t> lastLoggedAtMs_;
  uint64_t suppressed_{0};
};
