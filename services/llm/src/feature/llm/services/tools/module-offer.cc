#include "module-offer.hxx"

#include <algorithm>

namespace
{
std::string examplesOf(const ModuleIntroText& intro, const std::string& joiner)
{
  std::string out;
  for (const auto& example : intro.examples)
    out += (out.empty() ? "" : joiner) + example;
  return out;
}

std::string displayName(const ModuleFlag& module, bool english)
{
  const std::string& name = english ? module.name.en : module.name.es;
  return name.empty() ? module.id : name;
}

std::string whatIs(const ModuleFlag& module, bool english)
{
  const ModuleIntroText& intro = english ? module.intro.en : module.intro.es;
  if (!intro.what.empty())
    return intro.what;
  return english ? module.summary.en : module.summary.es;
}
}

const ModuleFlag* moduleNamed(const ModuleSnapshot& modules, const std::string& id)
{
  const auto found = std::ranges::find(modules.modules(), id, &ModuleFlag::id);
  return found == modules.modules().end() ? nullptr : &*found;
}

namespace
{
std::string introduction(const ModuleOfferInput& input)
{
  const bool english = input.lang == "en";
  const ModuleFlag* module = moduleNamed(input.audience.modules, input.module);
  const std::string name = module != nullptr ? displayName(*module, english) : input.module;
  const std::string what = module != nullptr ? whatIs(*module, english) : std::string();
  const std::string examples =
      module != nullptr ? examplesOf(english ? module->intro.en : module->intro.es, "; ") : std::string();
  std::string text = english ? "The " + name + " module is turned off. " : "El módulo " + name + " está apagado. ";
  if (!what.empty())
    text += what + " ";
  if (!examples.empty())
    text += (english ? "For example: " : "Por ejemplo: ") + examples + ". ";
  return text;
}
}

std::string moduleOfferFacts(const ModuleOfferInput& input)
{
  const bool english = input.lang == "en";
  const bool owner = input.audience.role == UserRole::Owner;
  std::string text = introduction(input);
  if (owner)
    text += english ? "If the user says yes, it can be turned on now." : "Si el usuario dice que sí, se puede activar ahora.";
  else
    text += english ? "If the user says yes, the owner of the house can be asked to turn it on."
                    : "Si el usuario dice que sí, se le puede pedir al dueño de la casa que lo active.";
  return text;
}

std::string moduleOfferText(const ModuleOfferInput& input)
{
  const bool english = input.lang == "en";
  const bool owner = input.audience.role == UserRole::Owner;
  std::string text = introduction(input);
  if (owner)
    text += english ? "Tell the user in a natural way that it is off and offer to turn it on. If they answer yes, call "
                      "modules.enable with module=" + input.module + ". Do not turn it on without their yes."
                    : "Dile al usuario con naturalidad que está apagado y ofrécele activarlo. Si responde que sí, "
                      "llama a modules.enable con module=" + input.module + ". No lo actives sin su sí.";
  else
    text += english ? "Tell the user in a natural way that it is off and offer to ask the owner of the house to turn "
                      "it on. If they answer yes, call modules.request with module=" + input.module + "."
                    : "Dile al usuario con naturalidad que está apagado y ofrécele pedírselo al dueño de la casa. Si "
                      "responde que sí, llama a modules.request con module=" + input.module + ".";
  return text;
}
