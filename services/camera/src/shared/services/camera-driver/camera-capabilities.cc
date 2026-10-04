#include "camera-capabilities.hxx"

#include <array>
#include <shared/services/camera-catalog/camera-catalog.hxx>
#include <shared/vocabulary/camera-stream-paths.hxx>
#include <string_view>
#include <text/json-util.hxx>

namespace
{
constexpr std::array<std::string_view, 12> kFlags{
    "ptz", "presets", "talk", "microphone", "privacy", "led",
    "dayNight", "motion", "autoTrack", "alarm", "sdCard", "streamOnly"};

const CameraCatalogEntry* entryOf(const CameraSchema& camera)
{
  return camera_catalog::find({.catalogId = camera_stream_paths::catalogIdOf(camera.config),
                               .driver = camera.driver,
                               .model = camera.model});
}

Json::Value tapoCapabilities(const CameraSchema& camera, const CameraCatalogEntry* entry)
{
  const auto has = [entry](CameraFeature feature) {
    return entry == nullptr || entry->has(feature);
  };
  Json::Value out;
  out["ptz"] = has(CameraFeature::Ptz);
  out["presets"] = has(CameraFeature::Presets);
  out["talk"] = has(CameraFeature::Speaker) && !camera.cloudPassword.empty();
  out["microphone"] = has(CameraFeature::Microphone);
  out["privacy"] = has(CameraFeature::Privacy);
  out["led"] = has(CameraFeature::Led);
  out["dayNight"] = has(CameraFeature::DayNight);
  out["motion"] = has(CameraFeature::Motion);
  out["autoTrack"] = has(CameraFeature::AutoTrack);
  out["alarm"] = has(CameraFeature::Siren);
  out["sdCard"] = has(CameraFeature::SdCard);
  out["streamOnly"] = false;
  return out;
}

Json::Value streamOnlyCapabilities(const CameraCatalogEntry* entry)
{
  Json::Value out;
  for (const auto flag : kFlags)
    out[std::string(flag)] = false;
  out["microphone"] = entry != nullptr && entry->has(CameraFeature::Microphone);
  out["streamOnly"] = true;
  return out;
}
}

namespace camera_capabilities
{
Json::Value of(const CameraSchema& camera)
{
  const auto* entry = entryOf(camera);
  Json::Value out = camera.driver == CameraDriver::Tapo ? tapoCapabilities(camera, entry)
                                                        : streamOnlyCapabilities(entry);
  out["catalogId"] = entry == nullptr ? std::string() : std::string(entry->id);
  return out;
}

std::string listOf(const Json::Value& capabilities)
{
  Json::Value list(Json::arrayValue);
  for (const auto flag : kFlags) {
    const std::string key(flag);
    if (capabilities.get(key, false).asBool())
      list.append(key);
  }
  return json_util::toString(list);
}
}
