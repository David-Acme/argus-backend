#include "link-status.hxx"

#include <json/json.h>

#include <utility>

namespace tunnel::link
{
routes::RemoteLink linkOf(const LinkSample& sample)
{
  if (sample.connected)
    return routes::RemoteLink::Up;
  return sample.attempts == 0 ? routes::RemoteLink::Connecting : routes::RemoteLink::Down;
}

LinkStatusTracker::LinkStatusTracker(int64_t startedAt) : since_(startedAt) {}

bool LinkStatusTracker::observe(const LinkSample& sample, int64_t now)
{
  const routes::RemoteLink next = linkOf(sample);
  if (next == link_)
    return false;
  link_ = next;
  since_ = now;
  return true;
}

std::optional<std::string> deviceDomainOf(std::string_view suffix)
{
  if (suffix.starts_with('.'))
    suffix.remove_prefix(1);
  if (!routes::isDomainName(suffix))
    return std::nullopt;
  return std::string(suffix);
}

LinkStatusPublisher::LinkStatusPublisher(LinkStatusPublisherDeps deps, LinkStatusPublisherOptions options)
    : deps_(std::move(deps)), options_(std::move(options)), tracker_(deps_.now())
{
}

void LinkStatusPublisher::tick()
{
  const int64_t now = deps_.now();
  if (tracker_.observe(deps_.sample(), now))
    pending_ = true;
  const bool due = now - lastPublishedAt_ >= options_.republish.count();
  if (!pending_ && !due)
    return;
  if (!deps_.publish(payload()))
    return;
  pending_ = false;
  lastPublishedAt_ = now;
}

routes::RemoteLinkState LinkStatusPublisher::state() const
{
  return {.homeId = options_.homeId, .domain = options_.domain, .link = tracker_.link(), .since = tracker_.since()};
}

std::string LinkStatusPublisher::payload() const
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, routes::remoteLinkToJson(state()));
}
}
