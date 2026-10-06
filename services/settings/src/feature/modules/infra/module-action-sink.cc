#include "module-action-sink.hxx"

#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

std::string module_action::msgId(std::int64_t auditId)
{
  return "settings-action:" + std::to_string(auditId);
}

NatsModuleActionSink::NatsModuleActionSink(NatsBus& bus) : bus_(bus) {}

bool NatsModuleActionSink::ensureStream()
{
  return bus_.ensureStream({.name = nats_subject::kSettingsActionStream,
                            .subjects = {nats_subject::kSettingsUserAction},
                            .maxAgeNs = module_action::kMaxAgeNs,
                            .duplicatesNs = module_action::kDuplicatesNs});
}

bool NatsModuleActionSink::publish(const ModuleActionRecord& record)
{
  if (!streamReady_.load(std::memory_order_acquire)) {
    if (!ensureStream())
      return false;
    streamReady_.store(true, std::memory_order_release);
  }
  const bool sent = bus_.publishWithMsgId({.subject = nats_subject::kSettingsUserAction,
                                           .payload = json_util::toString(record.event.toJson()),
                                           .msgId = module_action::msgId(record.auditId)});
  if (!sent) {
    streamReady_.store(false, std::memory_order_release);
    LOG_WARN << "Modules: the action journal entry " << record.auditId << " was not published";
  }
  return sent;
}
