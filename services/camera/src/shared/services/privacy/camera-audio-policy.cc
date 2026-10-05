#include "camera-audio-policy.hxx"

#include <algorithm>
#include <optional>
#include <runtime/blocking-task.hxx>
#include <utility>
#include <trantor/utils/Logger.h>

CameraAudioPolicy& CameraAudioPolicy::instance()
{
  static CameraAudioPolicy policy;
  return policy;
}

bool CameraAudioPolicy::allowedBy(
    const argus::identity::v1::ListPrivacyResponse& privacy)
{
  if (!privacy.household().camera_audio() || privacy.users().empty())
    return false;
  return std::ranges::all_of(privacy.users(), [](const auto& user) {
    return user.choices().decided() && user.choices().camera_audio();
  });
}

bool CameraAudioPolicy::apply(
    const argus::identity::v1::ListPrivacyResponse& privacy)
{
  const bool next = allowedBy(privacy);
  if (allowed() == next)
    return false;
  setAllowed(next);
  return true;
}

void CameraAudioPolicy::setAllowed(bool allowed)
{
  if (allowed_.exchange(allowed, std::memory_order_acq_rel) == allowed)
    return;
  LOG_INFO << "Camera audio " << (allowed ? "allowed" : "withheld")
           << " by the household's privacy choices";
  std::vector<Listener> listeners;
  {
    const std::scoped_lock lock(mutex_);
    listeners = listeners_;
  }
  for (const auto& listener : listeners)
    listener(allowed);
}

void CameraAudioPolicy::onChange(Listener listener)
{
  const std::scoped_lock lock(mutex_);
  listeners_.push_back(std::move(listener));
}

drogon::Task<void>
CameraAudioPolicy::refreshFrom(std::shared_ptr<const IdentityClient> client)
{
  if (!client)
    co_return;
  const auto privacy =
      co_await BlockingTask<std::optional<argus::identity::v1::ListPrivacyResponse>>(
          [client = std::move(client)]() { return client->listPrivacy(); });
  if (!privacy) {
    LOG_DEBUG << "Camera audio policy: identity did not answer; keeping "
              << (allowed() ? "allowed" : "withheld");
    co_return;
  }
  apply(*privacy);
}
