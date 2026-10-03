#pragma once

#include <shared/vocabulary/tool-contracts.hxx>

#include <string_view>
#include <vector>

[[nodiscard]] bool isAppTool(std::string_view name);

[[nodiscard]] std::vector<tools::ToolDescriptor> appToolDescriptors();
