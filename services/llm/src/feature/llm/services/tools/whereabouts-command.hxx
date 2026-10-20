#pragma once

#include <shared/vocabulary/tool-contracts.hxx>

#include <optional>
#include <string>

inline constexpr const char* kWhereaboutsTool = "guard.where_is";

[[nodiscard]] std::optional<tools::ToolCall> whereaboutsCommandFor(const std::string& utterance);
