#include "frame-lane.hxx"

#include <algorithm>
#include <utility>

FrameLane::FrameLane(FrameLaneConfig config, FrameLaneOwner owner)
    : config_(config), owner_(owner), tokens_(config.burst)
{
}

FrameAdmission FrameLane::admit(FrameJob job, double nowSeconds)
{
  std::scoped_lock lock(mutex_);
  if (closed_)
    return FrameAdmission::Stopping;
  if (refilledAt_) {
    const double elapsed = std::max(0.0, nowSeconds - *refilledAt_);
    tokens_ = std::min(config_.burst,
                       tokens_ + elapsed * config_.refillPerSecond);
  }
  refilledAt_ = nowSeconds;
  if (tokens_ < 1.0 || queue_.size() >= config_.maxQueued)
    return FrameAdmission::Refused;
  tokens_ -= 1.0;
  job.kind = FrameJobKind::Frame;
  return enqueueLocked(std::move(job));
}

FrameAdmission FrameLane::admitRevalidation()
{
  std::scoped_lock lock(mutex_);
  if (closed_)
    return FrameAdmission::Stopping;
  if (revalidationQueued_)
    return FrameAdmission::Refused;
  revalidationQueued_ = true;
  return enqueueLocked(FrameJob{.kind = FrameJobKind::Revalidate,
                                .message = Json::Value(),
                                .raw = {},
                                .type = {}});
}

FrameAdmission FrameLane::enqueueLocked(FrameJob job)
{
  queue_.push_back(std::move(job));
  if (draining_)
    return FrameAdmission::Queued;
  draining_ = true;
  return FrameAdmission::Start;
}

std::optional<FrameJob> FrameLane::next()
{
  std::scoped_lock lock(mutex_);
  if (queue_.empty()) {
    draining_ = false;
    return std::nullopt;
  }
  FrameJob job = std::move(queue_.front());
  queue_.pop_front();
  if (job.kind == FrameJobKind::Revalidate)
    revalidationQueued_ = false;
  return job;
}

void FrameLane::close()
{
  std::scoped_lock lock(mutex_);
  closed_ = true;
}

bool FrameLane::draining() const
{
  std::scoped_lock lock(mutex_);
  return draining_;
}
