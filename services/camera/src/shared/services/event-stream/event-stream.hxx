#pragma once

#include <cstdint>
#include <memory>
#include <string>

class NatsBus;

namespace camera_event_stream
{

inline constexpr const char* kName = "ARGUS_CAMERA";

inline constexpr int64_t kRetentionNs = 7LL * 24 * 60 * 60 * 1000000000;
inline constexpr int64_t kDuplicatesNs = 2LL * 60 * 1000000000;

struct EnsureInput
{
  std::string streamName;
  std::string changeSubject;
  std::string objectSubject;
};

[[nodiscard]] bool ensure(const std::shared_ptr<NatsBus>& bus,
                          const EnsureInput& input);

}
