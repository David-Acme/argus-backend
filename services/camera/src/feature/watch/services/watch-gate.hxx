#pragma once

#include <argus/camera/v1/watch.pb.h>

#include <string_view>

struct WatchGateInput
{
  bool known{false};
  bool enabled{false};
  bool moduleActive{false};
  bool audioAllowed{false};
  bool householdWatchman{false};
  bool cameraWatchman{false};
  bool noAudio{false};
  bool masked{false};
};

[[nodiscard]] argus::camera::v1::WatchRefusal watchGateVerdict(const WatchGateInput& input);

[[nodiscard]] std::string_view watchRefusalName(argus::camera::v1::WatchRefusal refusal);
