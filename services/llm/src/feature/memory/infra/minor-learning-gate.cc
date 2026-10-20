#include "minor-learning-gate.hxx"

#include <trantor/utils/Logger.h>

#include <utility>

namespace minor_learning
{

bool allows(const argus::identity::v1::ListPrivacyResponse& privacy, int64_t userId)
{
  for (const auto& user : privacy.users())
    if (user.user_id() == userId)
      return !user.choices().minor() || user.choices().guardian_consent();
  return true;
}

}

MinorLearningGate::MinorLearningGate(MinorLearningGateInput input) : input_(std::move(input))
{
  if (!input_.clock)
    input_.clock = [] { return std::chrono::steady_clock::now(); };
}

bool MinorLearningGate::learns(int64_t userId) const
{
  if (userId <= 0)
    return false;
  const auto now = input_.clock();
  std::scoped_lock lock(mutex_);
  if (!readAt_ || now - *readAt_ >= input_.refresh) {
    readAt_ = now;
    if (auto fresh = input_.fetch ? input_.fetch() : std::nullopt)
      snapshot_ = std::move(fresh);
    else
      LOG_WARN << "argus-llm: identity did not answer who is a minor; the last answer stands";
  }
  if (!snapshot_)
    return false;
  return minor_learning::allows(*snapshot_, userId);
}
