#pragma once

#include <feature/llm/services/tools/tool-access.hxx>

#include <string>

struct ModuleOfferInput
{
  const ToolAudience& audience;
  std::string module;
  std::string lang;
};

[[nodiscard]] std::string moduleOfferText(const ModuleOfferInput& input);

[[nodiscard]] const ModuleFlag* moduleNamed(const ModuleSnapshot& modules, const std::string& id);
