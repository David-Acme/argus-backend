#pragma once

#include <auth/capability.hxx>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace role_access
{

struct CapabilityFamily
{
  std::string_view capability;
  std::string_view es;
  std::string_view en;
};

inline constexpr std::array<CapabilityFamily, 3> kCapabilityFamilies{{
    {.capability = "projects.read", .es = "la agenda y las tareas", .en = "the calendar and tasks"},
    {.capability = "reminders.read", .es = "los recordatorios", .en = "reminders"},
    {.capability = "guard.read", .es = "la seguridad de la casa", .en = "home security"},
}};

struct RolePhrase
{
  UserRole role;
  std::string_view es;
  std::string_view en;
};

inline constexpr std::array<RolePhrase, 4> kRolePhrases{{
    {.role = UserRole::Owner, .es = "el propietario de la casa", .en = "the home's owner"},
    {.role = UserRole::Resident, .es = "un residente de la casa", .en = "a resident of the home"},
    {.role = UserRole::Guard, .es = "el guardia de la casa", .en = "the home's guard"},
    {.role = UserRole::Guest, .es = "un invitado", .en = "a guest"},
}};

inline std::string_view rolePhrase(UserRole role, bool english)
{
  const auto found = std::ranges::find(kRolePhrases, role, &RolePhrase::role);
  return found == kRolePhrases.end() ? std::string_view{} : (english ? found->en : found->es);
}

inline std::string joinPhrases(std::string_view prefix, const std::vector<std::string_view>& parts, bool english)
{
  std::string out(prefix);
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index > 0)
      out += index + 1 == parts.size() ? (english ? " and " : " y ") : ", ";
    out += parts[index];
  }
  return out + ".";
}

struct CapabilitySpeechInput
{
  std::string_view lang;
  UserRole role;
  const ModuleSnapshot& modules;
};

inline std::string capabilitySentence(const CapabilitySpeechInput& input)
{
  const bool english = input.lang == "en";
  const std::vector<std::string_view> granted =
      capabilitiesFor({.role = input.role, .modules = input.modules});
  std::vector<std::string_view> parts;
  for (const auto& family : kCapabilityFamilies)
    if (std::ranges::find(granted, family.capability) != granted.end())
      parts.push_back(english ? family.en : family.es);
  if (parts.empty())
    return {};
  return joinPhrases(english ? "you can help with " : "puedes ayudar con ", parts, english);
}

struct SpeakerLineInput
{
  std::string_view lang;
  UserRole role;
  std::string_view name;
  bool voiceCall{true};
};

inline std::string speakerLine(const SpeakerLineInput& input)
{
  const bool english = input.lang == "en";
  std::string who(input.name);
  const std::string_view phrase = rolePhrase(input.role, english);
  if (!phrase.empty()) {
    if (!who.empty())
      who += ", ";
    who += phrase;
  }
  std::string out = english ? (input.voiceCall ? "You are on a voice call" : "You are in a text chat")
                            : (input.voiceCall ? "Estás en una llamada de voz" : "Estás en un chat de texto");
  if (!who.empty())
    out += (english ? " with " : " con ") + who;
  return out + ".";
}

}
