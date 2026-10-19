#pragma once

#include <shared/vocabulary/tool-contracts.hxx>

#include <optional>
#include <string>

[[nodiscard]] std::optional<tools::ToolCall> sightCommandFor(const std::string& utterance);
