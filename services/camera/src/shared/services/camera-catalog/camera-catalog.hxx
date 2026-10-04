#pragma once

#include <camera/camera-driver.hxx>
#include <json/value.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

enum class CameraFormFactor : uint8_t
{
  PanTilt = 0,
  OutdoorPanTilt,
  Cube,
  Bullet,
  Turret,
  Dome,
  Doorbell
};

std::string_view cameraFormFactorToString(CameraFormFactor formFactor);

enum class CameraFeature : uint16_t
{
  Ptz = 1U << 0U,
  Presets = 1U << 1U,
  AutoTrack = 1U << 2U,
  Microphone = 1U << 3U,
  Speaker = 1U << 4U,
  Siren = 1U << 5U,
  Privacy = 1U << 6U,
  Led = 1U << 7U,
  DayNight = 1U << 8U,
  Motion = 1U << 9U,
  SdCard = 1U << 10U
};

constexpr uint16_t operator|(CameraFeature left, CameraFeature right)
{
  return static_cast<uint16_t>(static_cast<uint16_t>(left) | static_cast<uint16_t>(right));
}

constexpr uint16_t operator|(uint16_t left, CameraFeature right)
{
  return static_cast<uint16_t>(left | static_cast<uint16_t>(right));
}

struct CameraStreamDefaults
{
  int rtspPort{554};
  int onvifPort{0};
  std::string_view username;
  std::string_view streamPath;
  std::string_view subStreamPath;
};

struct CameraCatalogEntry
{
  std::string_view id;
  std::string_view brand;
  std::string_view manufacturer;
  std::string_view model;
  CameraDriver driver{CameraDriver::Rtsp};
  CameraFormFactor formFactor{CameraFormFactor::Bullet};
  bool outdoor{false};
  bool generic{false};
  std::string_view resolution;
  std::string_view subResolution;
  CameraStreamDefaults defaults;
  uint16_t features{0};
  std::string_view note;

  [[nodiscard]] constexpr bool has(CameraFeature feature) const
  {
    return (features & static_cast<uint16_t>(feature)) != 0;
  }

  [[nodiscard]] Json::Value toJson() const;
};

struct CameraCatalogLookup
{
  std::string_view catalogId;
  CameraDriver driver{CameraDriver::Tapo};
  std::string_view model;
};

namespace camera_catalog
{
std::span<const CameraCatalogEntry> entries();
const CameraCatalogEntry* byId(std::string_view id);
const CameraCatalogEntry* find(const CameraCatalogLookup& lookup);
Json::Value toJson();
}
