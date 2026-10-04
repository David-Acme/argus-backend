#pragma once

#include <feature/settings/services/settings-profile.hxx>
#include <runtime/hardware-profile.hxx>

[[nodiscard]] HardwareFacts hardwareFactsOf(const HardwareProfile& profile);
[[nodiscard]] HardwareFacts probeHardwareFacts();
