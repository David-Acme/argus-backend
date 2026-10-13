#pragma once

#include <routes/remote-link.hxx>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tunnel::link
{
struct LinkSample
{
  bool connected{false};
  int attempts{0};
};

[[nodiscard]] routes::RemoteLink linkOf(const LinkSample& sample);

class LinkStatusTracker
{
public:
  explicit LinkStatusTracker(int64_t startedAt);

  bool observe(const LinkSample& sample, int64_t now);

  [[nodiscard]] routes::RemoteLink link() const { return link_; }
  [[nodiscard]] int64_t since() const { return since_; }

private:
  routes::RemoteLink link_{routes::RemoteLink::Connecting};
  int64_t since_{0};
};

[[nodiscard]] std::optional<std::string> deviceDomainOf(std::string_view suffix);

struct LinkStatusPublisherDeps
{
  std::function<LinkSample()> sample;
  std::function<bool(std::string_view payload)> publish;
  std::function<int64_t()> now;
};

struct LinkStatusPublisherOptions
{
  std::string homeId;
  std::string domain;
  std::chrono::seconds republish{30};
};

class LinkStatusPublisher
{
public:
  LinkStatusPublisher(LinkStatusPublisherDeps deps, LinkStatusPublisherOptions options);

  void tick();

  [[nodiscard]] routes::RemoteLinkState state() const;
  [[nodiscard]] std::string payload() const;

private:
  LinkStatusPublisherDeps deps_;
  LinkStatusPublisherOptions options_;
  LinkStatusTracker tracker_;
  bool pending_{true};
  int64_t lastPublishedAt_{0};
};
}
