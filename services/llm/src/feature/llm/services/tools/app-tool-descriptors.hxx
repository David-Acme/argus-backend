#pragma once

#include <auth/role-access.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <optional>
#include <string_view>
#include <vector>

[[nodiscard]] bool isAppTool(std::string_view name);

[[nodiscard]] std::optional<role_access::AppAction> appActionOf(std::string_view name);

[[nodiscard]] std::vector<tools::ToolDescriptor> appToolDescriptors();
