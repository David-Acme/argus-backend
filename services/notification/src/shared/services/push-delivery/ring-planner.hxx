#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace push_delivery
{

struct RingPlanInput
{
  Json::Value data;
  std::vector<int64_t> userIds;
};

class RingPlanner
{
public:
  virtual ~RingPlanner() = default;

  [[nodiscard]] virtual drogon::Task<std::vector<int64_t>>
  ringFollowingUserIds(const RingPlanInput& input) const = 0;
};

class LateRingPlanner final : public RingPlanner
{
public:
  void bind(std::shared_ptr<const RingPlanner> planner) { planner_ = std::move(planner); }

  [[nodiscard]] drogon::Task<std::vector<int64_t>>
  ringFollowingUserIds(const RingPlanInput& input) const override
  {
    if (!planner_)
      co_return std::vector<int64_t>{};
    co_return co_await planner_->ringFollowingUserIds(input);
  }

private:
  std::shared_ptr<const RingPlanner> planner_;
};

}
