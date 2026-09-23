#include "event-stream.hxx"

#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <sync/stream-retention.hxx>
#include <trantor/utils/Logger.h>

#include <vector>

namespace camera_event_stream
{

bool ensure(const std::shared_ptr<NatsBus>& bus, const EnsureInput& input)
{
  if (!bus)
    return false;

  const bool overridden = !input.streamName.empty() ||
                          !input.changeSubject.empty() ||
                          !input.objectSubject.empty();
  std::vector<std::string> subjects;
  if (!overridden)
    subjects = {nats_subject::kCameraChange, nats_subject::kCameraObjectDetected};
  else if (!input.changeSubject.empty())
    subjects = {input.changeSubject};
  else if (!input.objectSubject.empty())
    subjects = {input.objectSubject};
  else
    subjects = {nats_subject::kCameraObjectDetected};

  const std::string stream =
      input.streamName.empty() ? std::string(kName) : input.streamName;
  if (!bus->ensureStream({.name = stream,
                          .subjects = subjects,
                          .maxAgeNs = stream_retention::kRetentionNs,
                          .duplicatesNs = stream_retention::kDuplicatesNs}))
    return false;
  LOG_INFO << "Camera JetStream: stream " << stream << " ready";
  return true;
}

}
