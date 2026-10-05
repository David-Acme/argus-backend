#include "nats-response-verdict-sink.hxx"

#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

NatsResponseVerdictSink::NatsResponseVerdictSink(std::shared_ptr<NatsBus> bus)
    : bus_(std::move(bus))
{
}

void NatsResponseVerdictSink::publish(const ResponseVerdictEvent& event) const
{
  if (!bus_)
    return;
  Json::Value payload(Json::objectValue);
  payload["responseId"] = static_cast<Json::Int64>(event.responseId);
  payload["kind"] = event.kind;
  payload["threadKey"] = event.threadKey;
  payload["episodeId"] = static_cast<Json::Int64>(event.episodeId);
  payload["environmentId"] = static_cast<Json::Int64>(event.environmentId);
  payload["verdict"] = event.verdict;
  payload["userId"] = static_cast<Json::Int64>(event.userId);
  payload["at"] = static_cast<Json::Int64>(event.at);
  if (!bus_->publish(nats_subject::kNotificationResponseVerdict,
                     json_util::toString(payload)))
    LOG_WARN << "Call engine: verdict for response " << event.responseId
             << " not published";
}
