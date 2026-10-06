#include "module-event-throttle.hxx"

bool ModuleEventThrottle::admit(const ThrottleSample& sample)
{
  const auto found = last_.find(sample.jobId);
  const bool admitted = found == last_.end() || found->second.state != sample.state ||
                        (sample.nowMs - found->second.atMs >= kMinIntervalMs &&
                         sample.progress - found->second.progress >= kMinStep);
  if (admitted)
    last_[sample.jobId] = {.state = sample.state, .progress = sample.progress, .atMs = sample.nowMs};
  if (jobTerminal(sample.state))
    last_.erase(sample.jobId);
  return admitted;
}
