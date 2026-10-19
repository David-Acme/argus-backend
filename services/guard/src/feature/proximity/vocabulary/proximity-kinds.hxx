#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ProximityReceiverKind : uint8_t
{
  Esphome = 0
};

enum class ProximitySensorModel : uint8_t
{
  Ld2410 = 0,
  Ld2450,
  Fp2
};

enum class ProximitySensorSource : uint8_t
{
  Esphome = 0,
  HomeAssistant
};

enum class ProximityEvidenceKind : uint8_t
{
  Face = 0,
  RadarBox,
  Voice,
  Ble
};

inline std::string proximityReceiverKindToString(ProximityReceiverKind kind)
{
  switch (kind) {
    case ProximityReceiverKind::Esphome:
      return "esphome";
  }
  return "esphome";
}

inline std::optional<ProximityReceiverKind> proximityReceiverKindFromString(std::string_view value)
{
  if (value == "esphome")
    return ProximityReceiverKind::Esphome;
  return std::nullopt;
}

inline std::string proximitySensorModelToString(ProximitySensorModel model)
{
  switch (model) {
    case ProximitySensorModel::Ld2410:
      return "ld2410";
    case ProximitySensorModel::Ld2450:
      return "ld2450";
    case ProximitySensorModel::Fp2:
      return "fp2";
  }
  return "ld2410";
}

inline std::optional<ProximitySensorModel> proximitySensorModelFromString(std::string_view value)
{
  if (value == "ld2410")
    return ProximitySensorModel::Ld2410;
  if (value == "ld2450")
    return ProximitySensorModel::Ld2450;
  if (value == "fp2")
    return ProximitySensorModel::Fp2;
  return std::nullopt;
}

inline std::string proximitySensorSourceToString(ProximitySensorSource source)
{
  switch (source) {
    case ProximitySensorSource::Esphome:
      return "esphome";
    case ProximitySensorSource::HomeAssistant:
      return "home_assistant";
  }
  return "esphome";
}

inline std::optional<ProximitySensorSource> proximitySensorSourceFromString(std::string_view value)
{
  if (value == "esphome")
    return ProximitySensorSource::Esphome;
  if (value == "home_assistant")
    return ProximitySensorSource::HomeAssistant;
  return std::nullopt;
}

inline std::string proximityEvidenceKindToString(ProximityEvidenceKind kind)
{
  switch (kind) {
    case ProximityEvidenceKind::Face:
      return "face";
    case ProximityEvidenceKind::RadarBox:
      return "radarBox";
    case ProximityEvidenceKind::Voice:
      return "voice";
    case ProximityEvidenceKind::Ble:
      return "ble";
  }
  return "ble";
}
