#pragma once

#include <shared/vocabulary/tool-contracts.hxx>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

[[nodiscard]] std::vector<std::string> spokenWords(const std::string& utterance);

[[nodiscard]] bool isAppTool(std::string_view name);

[[nodiscard]] std::optional<tools::ToolCall> appCommandFor(const std::string& utterance);

[[nodiscard]] bool asksForAppAction(const std::string& utterance);

[[nodiscard]] bool claimsAppAction(const std::string& reply);

[[nodiscard]] bool namesGuardMode(const std::string& utterance, std::string_view mode);
