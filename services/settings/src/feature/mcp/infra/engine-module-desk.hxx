#pragma once

#include <feature/mcp/services/module-desk.hxx>
#include <feature/modules/services/module-engine.hxx>

#include <string>
#include <vector>

struct EngineModuleDeskInput
{
  ModuleEngine* engine{nullptr};
};

class EngineModuleDesk final : public ModuleDesk
{
public:
  explicit EngineModuleDesk(EngineModuleDeskInput input);

  [[nodiscard]] drogon::Task<std::vector<ModuleCard>> list(const std::string& lang) override;
  [[nodiscard]] drogon::Task<std::optional<ModuleCard>> find(const DeskLookup& lookup) override;
  [[nodiscard]] drogon::Task<EnableOutcome> enable(const DeskCommand& command) override;
  [[nodiscard]] drogon::Task<ModuleImpact> impact(const DeskLookup& lookup) override;
  [[nodiscard]] drogon::Task<DisableOutcome> disable(const DeskCommand& command) override;
  [[nodiscard]] drogon::Task<RequestOutcome> request(const DeskCommand& command) override;

private:
  ModuleEngine* engine_;
};
