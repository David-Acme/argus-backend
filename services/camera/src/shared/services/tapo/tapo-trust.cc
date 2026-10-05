#include "tapo-trust.hxx"

#include <trantor/utils/Logger.h>

#include <utility>

TapoTrust::TapoTrust(TapoTrustState initial, Persist persist)
    : state_(std::move(initial)), persist_(std::move(persist))
{
}

TapoTrustState TapoTrust::state() const
{
  std::scoped_lock lock(mutex_);
  return state_;
}

std::string TapoTrust::pin() const
{
  std::scoped_lock lock(mutex_);
  return state_.fingerprint;
}

bool TapoTrust::secureSeen() const
{
  std::scoped_lock lock(mutex_);
  return state_.secure;
}

void TapoTrust::learn(std::string_view fingerprint)
{
  TapoTrustState learned;
  {
    std::scoped_lock lock(mutex_);
    if (fingerprint.empty() || !state_.fingerprint.empty())
      return;
    state_.fingerprint = std::string(fingerprint);
    learned = state_;
  }
  LOG_INFO << "tapo: pinned the camera's certificate on first use";
  if (persist_)
    persist_(learned);
}

void TapoTrust::noteSecure()
{
  TapoTrustState learned;
  {
    std::scoped_lock lock(mutex_);
    if (state_.secure)
      return;
    state_.secure = true;
    learned = state_;
  }
  if (persist_)
    persist_(learned);
}
