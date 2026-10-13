#include "remote-link-gate.hxx"

#include <json/json.h>

#include <memory>

namespace
{
std::optional<Json::Value> parsed(std::string_view payload)
{
  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value json;
  std::string errors;
  if (!reader->parse(payload.data(), payload.data() + payload.size(), &json, &errors))
    return std::nullopt;
  return json;
}
}

RemoteLinkGate::RemoteLinkGate(RemoteLinkGateConfig config) : config_(config) {}

routes::RemoteLinkRefusal RemoteLinkGate::ingest(std::string_view payload, int64_t nowMs)
{
  const auto json = parsed(payload);
  if (!json)
    return routes::RemoteLinkRefusal::NotAnObject;
  auto reading = routes::readRemoteLink(*json);
  if (!reading.state)
    return reading.refusal;
  state_ = std::move(reading.state);
  lastHeardAtMs_ = nowMs;
  return routes::RemoteLinkRefusal::None;
}

bool RemoteLinkGate::expire(int64_t nowMs)
{
  if (!state_ || state_->link == routes::RemoteLink::Down)
    return false;
  if (nowMs - lastHeardAtMs_ <= config_.staleAfterMs)
    return false;
  state_->link = routes::RemoteLink::Down;
  state_->since = lastHeardAtMs_ / 1000;
  return true;
}

std::optional<RemoteLinkGate::Visible> RemoteLinkGate::visible() const
{
  if (!state_)
    return std::nullopt;
  return Visible{.homeId = state_->homeId, .domain = state_->domain, .link = state_->link};
}

bool RemoteLinkGate::shouldSend(int64_t nowMs) const
{
  if (visible() == lastSent_)
    return false;
  return !lastSentAtMs_ || nowMs - *lastSentAtMs_ >= config_.throttleMs;
}

void RemoteLinkGate::markSent(int64_t nowMs)
{
  lastSent_ = visible();
  lastSentAtMs_ = nowMs;
}

RefusalLog::RefusalLog(int64_t windowMs) : windowMs_(windowMs) {}

std::optional<uint64_t> RefusalLog::record(int64_t nowMs)
{
  if (lastLoggedAtMs_ && nowMs - *lastLoggedAtMs_ < windowMs_) {
    ++suppressed_;
    return std::nullopt;
  }
  const uint64_t dropped = suppressed_ + 1;
  suppressed_ = 0;
  lastLoggedAtMs_ = nowMs;
  return dropped;
}
