#include "camera-catalog.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace
{
constexpr uint16_t kTapoFixed =
    CameraFeature::Microphone | CameraFeature::Speaker | CameraFeature::Siren |
    CameraFeature::Privacy | CameraFeature::Led | CameraFeature::DayNight |
    CameraFeature::Motion | CameraFeature::SdCard;
constexpr uint16_t kTapoPanTilt =
    kTapoFixed | CameraFeature::Ptz | CameraFeature::Presets | CameraFeature::AutoTrack;
constexpr uint16_t kTapoDoorbell = CameraFeature::Microphone | CameraFeature::Speaker;

constexpr std::string_view kTapoBrand = "TP-Link Tapo";
constexpr std::string_view kTapoManufacturer = "TP-Link";
constexpr std::string_view kTapoNote = "tapo-camera-account";
constexpr std::string_view k1080 = "1920x1080";
constexpr std::string_view k3mp = "2304x1296";
constexpr std::string_view k2k = "2560x1440";
constexpr std::string_view k4mp = "2688x1520";
constexpr std::string_view kSubLow = "640x360";
constexpr std::string_view kSubHd = "1280x720";

struct TapoSpec
{
  std::string_view id;
  std::string_view model;
  CameraFormFactor formFactor;
  bool outdoor;
  std::string_view resolution;
  uint16_t features;
  std::string_view note;
};

constexpr CameraCatalogEntry tapo(const TapoSpec& spec)
{
  return {.id = spec.id,
          .brand = kTapoBrand,
          .manufacturer = kTapoManufacturer,
          .model = spec.model,
          .driver = CameraDriver::Tapo,
          .formFactor = spec.formFactor,
          .outdoor = spec.outdoor,
          .generic = false,
          .resolution = spec.resolution,
          .subResolution = spec.resolution == k1080 ? kSubLow : kSubHd,
          .defaults = {.rtspPort = 554,
                       .onvifPort = 2020,
                       .username = "",
                       .streamPath = "/stream1",
                       .subStreamPath = "/stream2"},
          .features = spec.features,
          .note = spec.note};
}

struct GenericSpec
{
  std::string_view id;
  std::string_view brand;
  std::string_view manufacturer;
  CameraDriver driver;
  CameraFormFactor formFactor;
  CameraStreamDefaults defaults;
  std::string_view note;
};

constexpr CameraCatalogEntry generic(const GenericSpec& spec)
{
  return {.id = spec.id,
          .brand = spec.brand,
          .manufacturer = spec.manufacturer,
          .model = "",
          .driver = spec.driver,
          .formFactor = spec.formFactor,
          .outdoor = spec.formFactor != CameraFormFactor::Cube,
          .generic = true,
          .resolution = "",
          .subResolution = "",
          .defaults = spec.defaults,
          .features = 0,
          .note = spec.note};
}

constexpr CameraStreamDefaults kHikvisionPaths{.rtspPort = 554,
                                              .onvifPort = 80,
                                              .username = "admin",
                                              .streamPath = "/Streaming/Channels/101",
                                              .subStreamPath = "/Streaming/Channels/102"};
constexpr CameraStreamDefaults kDahuaPaths{
    .rtspPort = 554,
    .onvifPort = 80,
    .username = "admin",
    .streamPath = "/cam/realmonitor?channel=1&subtype=0",
    .subStreamPath = "/cam/realmonitor?channel=1&subtype=1"};

constexpr std::array kCatalog{
    tapo({.id = "tapo-c100", .model = "C100", .formFactor = CameraFormFactor::Cube, .outdoor = false, .resolution = k1080, .features = kTapoFixed, .note = kTapoNote}),
    tapo({.id = "tapo-c110", .model = "C110", .formFactor = CameraFormFactor::Cube, .outdoor = false, .resolution = k3mp, .features = kTapoFixed, .note = kTapoNote}),
    tapo({.id = "tapo-c120", .model = "C120", .formFactor = CameraFormFactor::Cube, .outdoor = true, .resolution = k2k, .features = kTapoFixed, .note = kTapoNote}),
    tapo({.id = "tapo-c125", .model = "C125", .formFactor = CameraFormFactor::Cube, .outdoor = false, .resolution = k2k, .features = kTapoFixed, .note = kTapoNote}),
    tapo({.id = "tapo-tc60", .model = "TC60", .formFactor = CameraFormFactor::Cube, .outdoor = false, .resolution = k1080, .features = kTapoFixed, .note = kTapoNote}),
    tapo({.id = "tapo-c200", .model = "C200", .formFactor = CameraFormFactor::PanTilt, .outdoor = false, .resolution = k1080, .features = kTapoPanTilt, .note = kTapoNote}),
    tapo({.id = "tapo-c210", .model = "C210", .formFactor = CameraFormFactor::PanTilt, .outdoor = false, .resolution = k3mp, .features = kTapoPanTilt, .note = kTapoNote}),
    tapo({.id = "tapo-c211", .model = "C211", .formFactor = CameraFormFactor::PanTilt, .outdoor = false, .resolution = k3mp, .features = kTapoPanTilt, .note = kTapoNote}),
    tapo({.id = "tapo-c220", .model = "C220", .formFactor = CameraFormFactor::PanTilt, .outdoor = false, .resolution = k2k, .features = kTapoPanTilt, .note = kTapoNote}),
    tapo({.id = "tapo-c225", .model = "C225", .formFactor = CameraFormFactor::PanTilt, .outdoor = false, .resolution = k4mp, .features = kTapoPanTilt, .note = kTapoNote}),
    tapo({.id = "tapo-tc70", .model = "TC70", .formFactor = CameraFormFactor::PanTilt, .outdoor = false, .resolution = k1080, .features = kTapoPanTilt, .note = kTapoNote}),
    tapo({.id = "tapo-c310", .model = "C310", .formFactor = CameraFormFactor::Bullet, .outdoor = true, .resolution = k3mp, .features = kTapoFixed, .note = kTapoNote}),
    tapo({.id = "tapo-c320ws", .model = "C320WS", .formFactor = CameraFormFactor::Bullet, .outdoor = true, .resolution = k2k, .features = kTapoFixed, .note = kTapoNote}),
    tapo({.id = "tapo-c325wb", .model = "C325WB", .formFactor = CameraFormFactor::Bullet, .outdoor = true, .resolution = k2k, .features = kTapoFixed, .note = kTapoNote}),
    tapo({.id = "tapo-c500", .model = "C500", .formFactor = CameraFormFactor::OutdoorPanTilt, .outdoor = true, .resolution = k1080, .features = kTapoPanTilt, .note = kTapoNote}),
    tapo({.id = "tapo-c510w", .model = "C510W", .formFactor = CameraFormFactor::OutdoorPanTilt, .outdoor = true, .resolution = k3mp, .features = kTapoPanTilt, .note = kTapoNote}),
    tapo({.id = "tapo-c520ws", .model = "C520WS", .formFactor = CameraFormFactor::OutdoorPanTilt, .outdoor = true, .resolution = k2k, .features = kTapoPanTilt, .note = kTapoNote}),
    tapo({.id = "tapo-d130", .model = "D130", .formFactor = CameraFormFactor::Doorbell, .outdoor = true, .resolution = k3mp, .features = kTapoDoorbell, .note = kTapoNote}),
    tapo({.id = "tapo-d225", .model = "D225", .formFactor = CameraFormFactor::Doorbell, .outdoor = true, .resolution = k2k, .features = kTapoDoorbell, .note = "tapo-doorbell-wired"}),
    generic({.id = "vigi", .brand = "TP-Link VIGI", .manufacturer = "TP-Link", .driver = CameraDriver::Onvif, .formFactor = CameraFormFactor::Turret, .defaults = {.rtspPort = 554, .onvifPort = 2020, .username = "admin", .streamPath = "/stream1", .subStreamPath = "/stream2"}, .note = ""}),
    generic({.id = "hikvision", .brand = "Hikvision", .manufacturer = "Hikvision", .driver = CameraDriver::Onvif, .formFactor = CameraFormFactor::Bullet, .defaults = kHikvisionPaths, .note = ""}),
    generic({.id = "annke", .brand = "Annke", .manufacturer = "Annke", .driver = CameraDriver::Onvif, .formFactor = CameraFormFactor::Turret, .defaults = kHikvisionPaths, .note = ""}),
    generic({.id = "dahua", .brand = "Dahua", .manufacturer = "Dahua", .driver = CameraDriver::Onvif, .formFactor = CameraFormFactor::Dome, .defaults = kDahuaPaths, .note = ""}),
    generic({.id = "amcrest", .brand = "Amcrest", .manufacturer = "Amcrest", .driver = CameraDriver::Onvif, .formFactor = CameraFormFactor::Turret, .defaults = kDahuaPaths, .note = ""}),
    generic({.id = "imou", .brand = "Imou", .manufacturer = "Imou", .driver = CameraDriver::Rtsp, .formFactor = CameraFormFactor::PanTilt, .defaults = kDahuaPaths, .note = "safety-code-password"}),
    generic({.id = "reolink", .brand = "Reolink", .manufacturer = "Reolink", .driver = CameraDriver::Onvif, .formFactor = CameraFormFactor::Bullet, .defaults = {.rtspPort = 554, .onvifPort = 8000, .username = "admin", .streamPath = "/h264Preview_01_main", .subStreamPath = "/h264Preview_01_sub"}, .note = "reolink-h265"}),
    generic({.id = "ezviz", .brand = "EZVIZ", .manufacturer = "EZVIZ", .driver = CameraDriver::Rtsp, .formFactor = CameraFormFactor::PanTilt, .defaults = {.rtspPort = 554, .onvifPort = 0, .username = "admin", .streamPath = "/h264/ch1/main/av_stream", .subStreamPath = "/h264/ch1/sub/av_stream"}, .note = "verification-code-password"}),
    generic({.id = "uniview", .brand = "Uniview", .manufacturer = "Uniview", .driver = CameraDriver::Onvif, .formFactor = CameraFormFactor::Turret, .defaults = {.rtspPort = 554, .onvifPort = 80, .username = "admin", .streamPath = "/media/video1", .subStreamPath = "/media/video2"}, .note = ""}),
    generic({.id = "axis", .brand = "Axis", .manufacturer = "Axis", .driver = CameraDriver::Onvif, .formFactor = CameraFormFactor::Dome, .defaults = {.rtspPort = 554, .onvifPort = 80, .username = "root", .streamPath = "/axis-media/media.amp", .subStreamPath = "/axis-media/media.amp?resolution=640x360"}, .note = ""}),
    generic({.id = "foscam", .brand = "Foscam", .manufacturer = "Foscam", .driver = CameraDriver::Rtsp, .formFactor = CameraFormFactor::PanTilt, .defaults = {.rtspPort = 88, .onvifPort = 888, .username = "admin", .streamPath = "/videoMain", .subStreamPath = "/videoSub"}, .note = ""}),
    generic({.id = "onvif", .brand = "ONVIF", .manufacturer = "", .driver = CameraDriver::Onvif, .formFactor = CameraFormFactor::Bullet, .defaults = {.rtspPort = 554, .onvifPort = 80, .username = "admin", .streamPath = "", .subStreamPath = ""}, .note = "custom-paths"}),
    generic({.id = "rtsp", .brand = "RTSP", .manufacturer = "", .driver = CameraDriver::Rtsp, .formFactor = CameraFormFactor::Bullet, .defaults = {.rtspPort = 554, .onvifPort = 0, .username = "", .streamPath = "", .subStreamPath = ""}, .note = "custom-paths"}),
};

std::string upper(std::string_view text)
{
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return out;
}

struct ModelMatch
{
  std::string_view text;
  std::string_view model;
};

bool namesModel(const ModelMatch& match)
{
  if (match.model.empty())
    return false;
  const std::string text = upper(match.text);
  const std::string needle = upper(match.model);
  size_t begin = 0;
  while (begin < text.size()) {
    while (begin < text.size() && std::isalnum(static_cast<unsigned char>(text[begin])) == 0)
      ++begin;
    size_t end = begin;
    while (end < text.size() && std::isalnum(static_cast<unsigned char>(text[end])) != 0)
      ++end;
    if (end > begin && std::string_view(text).substr(begin, end - begin) == needle)
      return true;
    begin = end;
  }
  return false;
}

Json::Value featuresJson(const CameraCatalogEntry& entry)
{
  Json::Value out(Json::objectValue);
  out["ptz"] = entry.has(CameraFeature::Ptz);
  out["presets"] = entry.has(CameraFeature::Presets);
  out["autoTrack"] = entry.has(CameraFeature::AutoTrack);
  out["microphone"] = entry.has(CameraFeature::Microphone);
  out["speaker"] = entry.has(CameraFeature::Speaker);
  out["siren"] = entry.has(CameraFeature::Siren);
  out["privacy"] = entry.has(CameraFeature::Privacy);
  out["led"] = entry.has(CameraFeature::Led);
  out["dayNight"] = entry.has(CameraFeature::DayNight);
  out["motion"] = entry.has(CameraFeature::Motion);
  out["sdCard"] = entry.has(CameraFeature::SdCard);
  return out;
}
}

std::string_view cameraFormFactorToString(CameraFormFactor formFactor)
{
  switch (formFactor) {
    case CameraFormFactor::PanTilt:
      return "pan-tilt";
    case CameraFormFactor::OutdoorPanTilt:
      return "outdoor-pan-tilt";
    case CameraFormFactor::Cube:
      return "cube";
    case CameraFormFactor::Bullet:
      return "bullet";
    case CameraFormFactor::Turret:
      return "turret";
    case CameraFormFactor::Dome:
      return "dome";
    case CameraFormFactor::Doorbell:
      return "doorbell";
  }
  return "bullet";
}

Json::Value CameraCatalogEntry::toJson() const
{
  Json::Value out(Json::objectValue);
  out["id"] = std::string(id);
  out["brand"] = std::string(brand);
  out["manufacturer"] = std::string(manufacturer);
  out["model"] = std::string(model);
  out["driver"] = cameraDriverToString(driver);
  out["formFactor"] = std::string(cameraFormFactorToString(formFactor));
  out["outdoor"] = outdoor;
  out["generic"] = generic;
  out["resolution"] = std::string(resolution);
  out["subResolution"] = std::string(subResolution);
  Json::Value stream(Json::objectValue);
  stream["port"] = defaults.rtspPort;
  stream["onvifPort"] = defaults.onvifPort;
  stream["username"] = std::string(defaults.username);
  stream["streamPath"] = std::string(defaults.streamPath);
  stream["subStreamPath"] = std::string(defaults.subStreamPath);
  out["defaults"] = stream;
  out["features"] = featuresJson(*this);
  out["note"] = std::string(note);
  return out;
}

std::span<const CameraCatalogEntry> camera_catalog::entries()
{
  return kCatalog;
}

const CameraCatalogEntry* camera_catalog::byId(std::string_view id)
{
  const auto* entry = std::ranges::find(kCatalog, id, &CameraCatalogEntry::id);
  return entry == kCatalog.end() ? nullptr : entry;
}

const CameraCatalogEntry* camera_catalog::find(const CameraCatalogLookup& lookup)
{
  if (const auto* entry = byId(lookup.catalogId); entry != nullptr && entry->driver == lookup.driver)
    return entry;
  const auto* entry = std::ranges::find_if(kCatalog, [&lookup](const CameraCatalogEntry& candidate) {
    return !candidate.generic && candidate.driver == lookup.driver &&
           namesModel({.text = lookup.model, .model = candidate.model});
  });
  return entry == kCatalog.end() ? nullptr : entry;
}

Json::Value camera_catalog::toJson()
{
  Json::Value models(Json::arrayValue);
  for (const auto& entry : kCatalog)
    models.append(entry.toJson());
  Json::Value out(Json::objectValue);
  out["models"] = models;
  return out;
}
