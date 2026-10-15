#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace push_delivery
{

struct RingPlanInput
{
  Json::Value data;
  int64_t userId{0};
};

class RingPlanner
{
public:
  virtual ~RingPlanner() = default;

  [[nodiscard]] virtual drogon::Task<bool> ringFollows(const RingPlanInput& input) const = 0;
};

class LateRingPlanner final : public RingPlanner
{
public:
  void bind(std::shared_ptr<const RingPlanner> planner) { planner_ = std::move(planner); }

  [[nodiscard]] drogon::Task<bool> ringFollows(const RingPlanInput& input) const override
  {
    if (!planner_)
      co_return false;
    co_return co_await planner_->ringFollows(input);
  }

private:
  std::shared_ptr<const RingPlanner> planner_;
};

}
