#pragma once

#include <argus/identity/v1/identity.pb.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace minor_learning
{

[[nodiscard]] bool allows(const argus::identity::v1::ListPrivacyResponse& privacy, int64_t userId);

}

struct MinorLearningGateInput
{
  std::function<std::optional<argus::identity::v1::ListPrivacyResponse>()> fetch;
  std::chrono::milliseconds refresh{60000};
  std::function<std::chrono::steady_clock::time_point()> clock;
};

class MinorLearningGate
{
public:
  explicit MinorLearningGate(MinorLearningGateInput input);

  [[nodiscard]] bool learns(int64_t userId) const;

private:
  MinorLearningGateInput input_;
  mutable std::mutex mutex_;
  mutable std::optional<argus::identity::v1::ListPrivacyResponse> snapshot_;
  mutable std::optional<std::chrono::steady_clock::time_point> readAt_;
};
