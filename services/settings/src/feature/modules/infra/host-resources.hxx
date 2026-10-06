#pragma once

#include <feature/modules/services/hardware-check.hxx>
#include <runtime/hardware-profile.hxx>

#include <cstdint>
#include <optional>
#include <string>

[[nodiscard]] std::optional<std::int64_t> freeDiskBytes(const std::string& directory);

[[nodiscard]] HostResources hostResourcesOf(const HardwareProfile& profile);
