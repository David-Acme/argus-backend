#include "nats-response-verdict-sink.hxx"

#include <drogon/drogon.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/blocking-task.hxx>
#include <sync/stream-retention.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

namespace
{
bool ensureVerdictStream(NatsBus& bus)
{
  return bus.ensureStream({.name = nats_subject::kNotificationVerdictStream,
                           .subjects = {nats_subject::kNotificationResponseVerdict},
                           .maxAgeNs = stream_retention::kRetentionNs,
                           .duplicatesNs = stream_retention::kDuplicatesNs});
}

struct StoreInput
{
  NatsBus& bus;
  std::atomic<bool>& streamReady;
  const ResponseVerdictEvent& event;
};

bool store(const StoreInput& input)
{
  if (!input.streamReady.load(std::memory_order_acquire))
    input.streamReady.store(ensureVerdictStream(input.bus), std::memory_order_release);
  const NatsBus::PublishWithIdInput message{
      .subject = nats_subject::kNotificationResponseVerdict,
      .payload = response_verdict_wire::payload(input.event),
      .msgId = response_verdict_wire::messageId(input.event)};
  if (input.bus.publishWithMsgId(message))
    return true;
  input.streamReady.store(ensureVerdictStream(input.bus), std::memory_order_release);
  return input.bus.publishWithMsgId(message);
}
}

std::string response_verdict_wire::messageId(const ResponseVerdictEvent& event)
{
  return "response-verdict:" + std::to_string(event.responseId) + ":" + event.verdict + ":" +
         std::to_string(event.userId);
}

std::string response_verdict_wire::payload(const ResponseVerdictEvent& event)
{
  Json::Value payload(Json::objectValue);
  payload["responseId"] = static_cast<Json::Int64>(event.responseId);
  payload["kind"] = event.kind;
  payload["threadKey"] = event.threadKey;
  payload["episodeId"] = static_cast<Json::Int64>(event.episodeId);
  payload["environmentId"] = static_cast<Json::Int64>(event.environmentId);
  payload["verdict"] = event.verdict;
  payload["userId"] = static_cast<Json::Int64>(event.userId);
  payload["at"] = static_cast<Json::Int64>(event.at);
  return json_util::toString(payload);
}

NatsResponseVerdictSink::NatsResponseVerdictSink(std::shared_ptr<NatsBus> bus)
    : bus_(std::move(bus)), streamReady_(std::make_shared<std::atomic<bool>>(false))
{
  if (bus_ && bus_->isConnected())
    streamReady_->store(ensureVerdictStream(*bus_), std::memory_order_release);
}

void NatsResponseVerdictSink::publish(const ResponseVerdictEvent& event) const
{
  if (!bus_)
    return;
  drogon::async_run([bus = bus_, ready = streamReady_, event]() -> drogon::Task<void> {
    const bool stored = co_await BlockingTask<bool>{[bus, ready, event]() {
      return store({.bus = *bus, .streamReady = *ready, .event = event});
    }};
    if (!stored)
      LOG_ERROR << "Call engine: verdict for response " << event.responseId
                << " could not be stored in " << nats_subject::kNotificationVerdictStream;
  });
}
