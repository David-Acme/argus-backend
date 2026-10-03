#pragma once

#include <shared/vocabulary/tool-contracts.hxx>

#include <optional>
#include <string>

[[nodiscard]] std::optional<tools::ToolCall> appCommandFor(const std::string& utterance);
