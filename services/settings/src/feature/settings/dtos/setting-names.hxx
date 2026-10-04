#pragma once

#include <config/settings-registry.hxx>

#include <string>

[[nodiscard]] std::string settingTypeName(SettingType type);
[[nodiscard]] std::string settingLevelName(SettingLevel level);
[[nodiscard]] std::string settingApplyName(SettingApply apply);
[[nodiscard]] std::string choiceAvailabilityName(ChoiceAvailability availability);
[[nodiscard]] std::string rejectionReasonName(SettingRejectionReason reason);
