#pragma once

#include <auth/module-snapshot.hxx>

#include <string>

namespace spoken_intent
{

struct ModuleMention
{
  const std::string& utterance;
  const ModuleFlag& module;
};

[[nodiscard]] bool affirms(const std::string& utterance);

[[nodiscard]] bool asksToEnable(const ModuleMention& mention);

[[nodiscard]] bool asksToRequest(const ModuleMention& mention);

}
