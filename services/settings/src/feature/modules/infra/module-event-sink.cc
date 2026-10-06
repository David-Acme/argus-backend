#include "module-event-sink.hxx"

#include <feature/modules/dtos/module-json.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>

namespace
{
std::int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Json::Value envelope(std::string_view kind, const ModuleStatesReply& set, std::int64_t atMs)
{
  Json::Value json(Json::objectValue);
  json["kind"] = std::string(kind);
  json["version"] = static_cast<Json::Int64>(set.version);
  json["settled"] = set.settled;
  json["at"] = static_cast<Json::Int64>(atMs);
  return json;
}
}

namespace module_event
{
std::string modulePayload(const ModuleView& view, const ModuleStatesReply& set, std::int64_t atMs)
{
  auto json = envelope("module", set, atMs);
  json["module"] = module_json::module(view, "es");
  return json_util::toString(json);
}

std::string enabledPayload(const ModuleStatesReply& set, std::int64_t atMs)
{
  auto json = envelope("enabled", set, atMs);
  json["modules"] = module_json::enabledModules(set);
  return json_util::toString(json);
}
}

NatsModuleEventSink::NatsModuleEventSink(NatsBus& bus) : bus_(bus), bootMs_(nowMs()) {}

bool NatsModuleEventSink::ensureStream()
{
  return bus_.ensureStream({.name = nats_subject::kSettingsModuleStream,
                            .subjects = {nats_subject::kSettingsModule},
                            .maxAgeNs = module_event::kMaxAgeNs,
                            .duplicatesNs = module_event::kDuplicatesNs});
}

bool NatsModuleEventSink::publish(std::string payload)
{
  const auto sequence = ++sequence_;
  const bool sent = bus_.publishWithMsgId({.subject = nats_subject::kSettingsModule,
                                           .payload = std::move(payload),
                                           .msgId = std::to_string(bootMs_) + "-" + std::to_string(sequence)});
  if (!sent)
    LOG_WARN << "Modules: the module event " << sequence << " was not published";
  return sent;
}

bool NatsModuleEventSink::moduleChanged(const ModuleView& view, const ModuleStatesReply& set)
{
  return publish(module_event::modulePayload(view, set, nowMs()));
}

bool NatsModuleEventSink::enabledChanged(const ModuleStatesReply& set)
{
  return publish(module_event::enabledPayload(set, nowMs()));
}
