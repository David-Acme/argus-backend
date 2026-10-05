#pragma once

#include <argus/identity/v1/identity.pb.h>
#include <atomic>
#include <drogon/utils/coroutine.h>
#include <identity/identity-client.hxx>
#include <memory>
#include <functional>
#include <mutex>
#include <vector>

class CameraAudioPolicy
{
public:
  using Listener = std::function<void(bool)>;

  static CameraAudioPolicy& instance();

  [[nodiscard]] static bool
  allowedBy(const argus::identity::v1::ListPrivacyResponse& privacy);

  [[nodiscard]] bool allowed() const
  {
    return allowed_.load(std::memory_order_acquire);
  }

  bool apply(const argus::identity::v1::ListPrivacyResponse& privacy);

  void setAllowed(bool allowed);

  void onChange(Listener listener);

  drogon::Task<void> refreshFrom(std::shared_ptr<const IdentityClient> client);

private:
  CameraAudioPolicy() = default;

  std::atomic<bool> allowed_{false};
  std::mutex mutex_;
  std::vector<Listener> listeners_;
};
