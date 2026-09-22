#pragma once

#include <cstdint>
#include <memory>
#include <string>

class NatsBus;

// The one JetStream stream the camera service publishes over. The change feed
// (argus.camera.v1.change) and the object feed
// (argus.camera.v1.object_detected) share ARGUS_CAMERA, so both sinks declare
// the same subject set through here; a stream is reconciled by comparing its
// subjects, and two declares that disagreed would refuse each other for ever.
namespace camera_event_stream
{

inline constexpr const char* kName = "ARGUS_CAMERA";

// 7 days of server-side retention, in nanoseconds.
inline constexpr int64_t kRetentionNs = 7LL * 24 * 60 * 60 * 1000000000;
inline constexpr int64_t kDuplicatesNs = 2LL * 60 * 1000000000;

// Empty fields keep the production stream and its subject pair; an override
// narrows the declaration to the one subject it names.
struct EnsureInput
{
  std::string streamName;
  std::string changeSubject;
  std::string objectSubject;
};

// Creates the stream when missing and reconciles it when present; true on
// success. Idempotent, so a refused publish re-runs it rather than latching.
[[nodiscard]] bool ensure(const std::shared_ptr<NatsBus>& bus,
                          const EnsureInput& input);

} // namespace camera_event_stream
