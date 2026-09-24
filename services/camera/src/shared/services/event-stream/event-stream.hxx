#pragma once

#include <cstdint>
#include <memory>
#include <string>

class NatsBus;

namespace camera_event_stream
{

struct EnsureInput
{
  std::string streamName;
  std::string changeSubject;
  std::string objectSubject;
};

[[nodiscard]] bool ensure(const std::shared_ptr<NatsBus>& bus,
                          const EnsureInput& input);

}
