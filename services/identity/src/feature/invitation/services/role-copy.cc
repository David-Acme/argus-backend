#include "role-copy.hxx"

namespace
{
constexpr std::string_view kEnglish = "en";
}

namespace role_copy
{
std::string label(UserRole role, std::string_view lang)
{
  const bool english = lang == kEnglish;
  switch (role) {
    case UserRole::Owner:
      return english ? "Owner" : "Propietario";
    case UserRole::Resident:
      return english ? "Resident" : "Residente";
    case UserRole::Guard:
      return english ? "Guard" : "Vigilante";
    case UserRole::Guest:
      return english ? "Guest" : "Invitado";
    case UserRole::Unknown:
      break;
  }
  return english ? "This role" : "Este rol";
}

std::string needsModule(UserRole role, const ModuleFlag& module, std::string_view lang)
{
  const bool english = lang == kEnglish;
  const std::string& named = english ? module.name.en : module.name.es;
  const std::string name = named.empty() ? module.id : named;
  if (english)
    return label(role, lang) + " needs the " + name + " module";
  return label(role, lang) + " necesita el módulo " + name;
}
}
