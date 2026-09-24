#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <feature/fanout/services/durable-delivery.hxx>
#include <feature/fanout/services/durable-disposition.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class NatsBus;

namespace change_feed
{
struct Feed
{
  std::string stream;
  std::string subject;
  std::string durable;
};

const std::vector<Feed>& defaults();
}

class ChangeFeedConsumer
{
public:
  struct Dependencies
  {
    NatsBus* bus{nullptr};
    AuditFanOut* auditFanOut{nullptr};
  };

  struct Config
  {
    std::vector<change_feed::Feed> feeds{change_feed::defaults()};
    int maxDeliver{50};
  };

  ChangeFeedConsumer(Dependencies dependencies, Config config);
  ~ChangeFeedConsumer();

  void start();
  void stop();

  drogon::Task<DurableDisposition>
  handle(const durable_delivery::Payload& message);

private:
  struct Attachment
  {
    change_feed::Feed feed;
    std::optional<uint64_t> subscription;
  };

  bool trySubscribe(Attachment& attachment);
  bool subscribePending();
  void scheduleSubscribeRetry();

  Dependencies dependencies_;
  Config config_;
  std::vector<Attachment> attachments_;
  std::optional<uint64_t> retryTimer_;
};
