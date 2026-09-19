#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// One physical or user-visible effect the guard may raise; the authorizer owns
// the mapping.
enum class GuardActionKind : uint8_t
{
  Greet = 0,
  Listen,
  Reply,
  Announce,
  Alarm,
  SirenArm,
  SirenDisarm,
  Notify
};

inline std::string guardActionKindToString(GuardActionKind kind)
{
  switch (kind) {
    case GuardActionKind::Greet:
      return "greet";
    case GuardActionKind::Listen:
      return "greet_listen";
    case GuardActionKind::Reply:
      return "greet_reply";
    case GuardActionKind::Announce:
      return "announce";
    case GuardActionKind::Alarm:
      return "alarm";
    case GuardActionKind::SirenArm:
      return "siren_arm";
    case GuardActionKind::SirenDisarm:
      return "siren_disarm";
    case GuardActionKind::Notify:
      return "notify";
  }
  return "notify";
}

inline GuardActionKind guardActionKindFromString(const std::string& value)
{
  static const std::unordered_map<std::string, GuardActionKind> kMap = {
      {"greet", GuardActionKind::Greet},
      {"greet_listen", GuardActionKind::Listen},
      {"greet_reply", GuardActionKind::Reply},
      {"announce", GuardActionKind::Announce},
      {"alarm", GuardActionKind::Alarm},
      {"siren_arm", GuardActionKind::SirenArm},
      {"siren_disarm", GuardActionKind::SirenDisarm},
      {"notify", GuardActionKind::Notify},
  };
  const auto found = kMap.find(value);
  return found == kMap.end() ? GuardActionKind::Notify : found->second;
}
