#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class PushProvider : uint8_t
{
  Fcm = 1,
  Apns = 2,
  ApnsVoip = 3
};

inline std::string_view pushProviderToString(PushProvider provider) noexcept
{
  switch (provider) {
    case PushProvider::Fcm:
      return "fcm";
    case PushProvider::Apns:
      return "apns";
    case PushProvider::ApnsVoip:
      return "apns_voip";
  }
  return "";
}

inline std::optional<PushProvider>
pushProviderFromString(std::string_view name) noexcept
{
  if (name == "fcm")
    return PushProvider::Fcm;
  if (name == "apns")
    return PushProvider::Apns;
  if (name == "apns_voip")
    return PushProvider::ApnsVoip;
  return std::nullopt;
}
