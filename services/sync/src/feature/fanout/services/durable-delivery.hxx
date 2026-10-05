#pragma once

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <feature/fanout/services/durable-disposition.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <trantor/utils/Logger.h>
#include <utility>

namespace durable_delivery
{
struct Payload
{
  std::string subject;
  std::string msgId;
  std::string body;
};

struct Pending
{
  Payload payload;
  NatsBus::DurableSettlement settlement;
};

using PendingCount = std::shared_ptr<std::atomic<int64_t>>;

struct HandlerOptions
{
  std::string label;
  PendingCount pending;
};

template <typename Runner>
struct SerialFeed
{
  std::string label;
  Runner runner;
  std::deque<Pending> queue;
  bool draining{false};
  PendingCount pending;
};

inline void settle(const NatsBus::DurableSettlement& settlement,
                   DurableDisposition disposition)
{
  switch (disposition) {
    case DurableDisposition::Ack:
      if (settlement.ack)
        settlement.ack();
      return;
    case DurableDisposition::Term:
      if (settlement.term)
        settlement.term();
      return;
    case DurableDisposition::Nak:
      if (settlement.nak)
        settlement.nak();
      return;
  }
}

template <typename Runner>
drogon::Task<void> drain(std::shared_ptr<SerialFeed<Runner>> feed)
{
  trantor::EventLoop* const loop = drogon::app().getIOLoop(0);
  while (!feed->queue.empty()) {
    Pending next = std::move(feed->queue.front());
    feed->queue.pop_front();
    DurableDisposition disposition = DurableDisposition::Nak;
    try {
      disposition = co_await feed->runner(next.payload);
    }
    catch (const std::exception& error) {
      LOG_WARN << feed->label << ": redelivering (" << error.what() << ")";
    }
    settle(next.settlement, disposition);
    if (feed->pending)
      feed->pending->fetch_sub(1, std::memory_order_acq_rel);
    if (!loop->isInLoopThread())
      co_await drogon::switchThreadCoro(loop);
  }
  feed->draining = false;
}

template <typename Runner>
NatsBus::DurableHandler handler(const HandlerOptions& options, Runner runner)
{
  auto feed = std::make_shared<SerialFeed<Runner>>(
      SerialFeed<Runner>{.label = options.label,
                         .runner = std::move(runner),
                         .queue = {},
                         .draining = false,
                         .pending = options.pending});
  return [feed](const NatsBus::DurableMessage& message,
                NatsBus::DurableSettlement settlement) {
    if (feed->pending)
      feed->pending->fetch_add(1, std::memory_order_acq_rel);
    Pending pending{.payload = {.subject = std::string(message.subject),
                                .msgId = std::string(message.msgId),
                                .body = std::string(message.payload)},
                    .settlement = std::move(settlement)};
    drogon::app().getIOLoop(0)->runInLoop(
        [feed, pending = std::move(pending)]() mutable {
          feed->queue.push_back(std::move(pending));
          if (feed->draining)
            return;
          feed->draining = true;
          drogon::async_run([feed]() { return drain(feed); });
        });
  };
}
}
