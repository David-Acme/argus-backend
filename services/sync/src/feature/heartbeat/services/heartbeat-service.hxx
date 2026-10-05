#pragma once

#include <feature/heartbeat/services/heartbeat-policy.hxx>
#include <feature/heartbeat/services/presence-board.hxx>
#include <feature/transport/infra/heartbeat-source.hxx>

#include <cstdint>
#include <functional>
#include <json/value.h>
#include <memory>

class HeartbeatService : public HeartbeatSource
{
public:
  using Clock = std::function<int64_t()>;

  struct Dependencies
  {
    std::shared_ptr<const PresenceBoard> board;
    Clock clock;
  };

  HeartbeatService(Dependencies dependencies, HeartbeatPolicy policy);

  [[nodiscard]] Json::Value heartbeatFor(int64_t userId) const override;
  [[nodiscard]] Json::Value frameFor(int64_t userId) const;
  [[nodiscard]] const HeartbeatPolicy& policy() const { return policy_; }

private:
  Dependencies dependencies_;
  HeartbeatPolicy policy_;
};
