#include <operator/operator-config.hxx>

#include <shared/services/config-service/config-service.hxx>
#include <shared/utils/json-util/json-util.hxx>

#include <json/value.h>

// The class table must match the model's output width (row length = 4 + class count).
std::vector<std::string> operator_config::defaultClasses()
{
  return {"person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
          "truck", "boat", "traffic light", "fire hydrant", "stop sign",
          "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep",
          "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
          "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
          "sports ball", "kite", "baseball bat", "baseball glove", "skateboard",
          "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
          "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
          "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
          "couch", "potted plant", "bed", "dining table", "toilet", "tv",
          "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave",
          "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
          "scissors", "teddy bear", "hair drier", "toothbrush"};
}

namespace
{
std::vector<std::string> splitCsv(const std::string& value)
{
  std::vector<std::string> items;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t comma = value.find(',', start);
    std::string item = comma == std::string::npos
                           ? value.substr(start)
                           : value.substr(start, comma - start);
    while (!item.empty() && item.front() == ' ')
      item.erase(item.begin());
    while (!item.empty() && item.back() == ' ')
      item.pop_back();
    if (!item.empty())
      items.push_back(item);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return items;
}
} // namespace

ObjectsConfig operator_config::resolveObjects()
{
  ObjectsConfig config;
  config.enabled = ConfigService::getBool("objects.enabled");
  config.model = ConfigService::getString("objects.model");
  if (config.model.empty())
    config.model = "models/objects";
  config.classes = splitCsv(ConfigService::getString("objects.classes"));
  if (config.classes.empty())
    config.classes = defaultClasses();
  config.inputSize = ConfigService::getInt("objects.input_size");
  if (config.inputSize <= 0)
    config.inputSize = 640;
  config.confidence =
      static_cast<float>(ConfigService::getDouble("objects.conf"));
  if (config.confidence <= 0.f)
    config.confidence = 0.45f;
  config.maxFpsInference = ConfigService::getDouble("objects.max_fps_inference");
  if (config.maxFpsInference <= 0)
    config.maxFpsInference = 2.0;
  config.activeFps = ConfigService::getDouble("objects.active_fps");
  if (config.activeFps <= 0)
    config.activeFps = 6.0;
  config.burstFps = ConfigService::getDouble("objects.burst_fps");
  if (config.burstFps <= 0)
    config.burstFps = 10.0;
  config.burstMs = ConfigService::getInt("objects.burst_ms");
  if (config.burstMs <= 0)
    config.burstMs = 3000;
  config.useVulkan = ConfigService::getBool("objects.use_vulkan");
  return config;
}

OperatorConfig operator_config::resolveOperator()
{
  OperatorConfig config;
  config.overlay = ConfigService::getBool("operator.overlay");
  config.overlayDir = ConfigService::getString("operator.overlay_dir");
  config.aggregationWindowMs =
      ConfigService::getInt("operator.aggregation_window_ms");
  if (config.aggregationWindowMs < 0)
    config.aggregationWindowMs = 5000;
  config.cooldownMs = ConfigService::getInt("operator.cooldown_ms");
  if (config.cooldownMs < 0)
    config.cooldownMs = 30000;
  config.nightStartHour = ConfigService::getInt("operator.night_start");
  if (config.nightStartHour < 0)
    config.nightStartHour = 22;
  config.nightEndHour = ConfigService::getInt("operator.night_end");
  if (config.nightEndHour < 0)
    config.nightEndHour = 6;
  config.presenceEscalationFrames =
      ConfigService::getInt("operator.presence_escalation_frames");
  if (config.presenceEscalationFrames <= 0)
    config.presenceEscalationFrames = 3;
  config.ignoredClasses =
      splitCsv(ConfigService::getString("operator.ignored_classes"));

  config.zonesFromDb =
      !ConfigService::hasKey("operator.zones_from_db") ||
      ConfigService::getBool("operator.zones_from_db");
  config.zonesRefreshMs = ConfigService::getInt("operator.zones_refresh_ms");
  if (config.zonesRefreshMs <= 0)
    config.zonesRefreshMs = 30000;
  config.cameraRescanMs = ConfigService::getInt("operator.camera_rescan_ms");
  if (config.cameraRescanMs <= 0)
    config.cameraRescanMs = 15000;
  config.motionGate = ConfigService::getBool("operator.motion_gate");
  config.motionMinRatio = ConfigService::getDouble("operator.motion_min_ratio");
  if (config.motionMinRatio <= 0)
    config.motionMinRatio = 0.002;
  config.ignoreStaticPersons =
      ConfigService::getBool("operator.ignore_static_persons");
  config.staticBoxFrames = ConfigService::getInt("operator.static_box_frames");
  if (config.staticBoxFrames <= 0)
    config.staticBoxFrames = 8;
  config.dwellAlertMs = ConfigService::getInt("operator.dwell_alert_ms");
  if (config.dwellAlertMs < 0)
    config.dwellAlertMs = 3000;
  if (ConfigService::hasKey("operator.dwell_monitor_ms")) {
    const int value = ConfigService::getInt("operator.dwell_monitor_ms");
    config.dwellMonitorMs = value >= 0 ? value : 12000;
  }
  else
    config.dwellMonitorMs = 12000;
  if (ConfigService::hasKey("operator.dwell_night_ms")) {
    const int value = ConfigService::getInt("operator.dwell_night_ms");
    config.dwellNightMs = value >= 0 ? value : 8000;
  }
  else
    config.dwellNightMs = 8000;
  config.personRecheckMs = ConfigService::getInt("operator.person_recheck_ms");
  if (config.personRecheckMs <= 0)
    config.personRecheckMs = 30000;
  config.trackTtlMs = ConfigService::getInt("operator.track_ttl_ms");
  if (config.trackTtlMs <= 0)
    config.trackTtlMs = 3000;
  config.trackIouMin = ConfigService::getDouble("operator.track_iou_min");
  if (config.trackIouMin <= 0)
    config.trackIouMin = 0.3;

  const std::string zonesJson = ConfigService::getString("operator.zones");
  if (!zonesJson.empty()) {
    const Json::Value zones = json_util::fromString(zonesJson);
    if (zones.isArray()) {
      for (const auto& zone : zones) {
        OperatorZone parsed;
        parsed.cameraId = zone.get("cameraId", 0).asInt64();
        parsed.name = zone.get("name", "").asString();
        parsed.kind = zone.get("kind", "").asString();
        const Json::Value points = zone.get("points", Json::Value());
        if (points.isArray()) {
          for (const auto& point : points) {
            if (point.isArray() && point.size() >= 2)
              parsed.points.emplace_back(point[0].asDouble(), point[1].asDouble());
          }
        }
        if (parsed.cameraId > 0 && !parsed.name.empty() && parsed.points.size() >= 3)
          config.zones.push_back(std::move(parsed));
      }
    }
  }
  return config;
}

IdentityConfig operator_config::resolveIdentity()
{
  IdentityConfig config;
  config.identify = ConfigService::getBool("identity.identify");
  config.autoEnroll = ConfigService::getBool("identity.auto_enroll");
  config.captureClearFaces =
      !ConfigService::hasKey("identity.capture_clear_faces") ||
      ConfigService::getBool("identity.capture_clear_faces");
  config.minFaceBoxPx = ConfigService::getInt("identity.min_face_box_px");
  if (config.minFaceBoxPx <= 0)
    config.minFaceBoxPx = 48;
  config.identifyIntervalMs =
      ConfigService::getInt("identity.identify_interval_ms");
  if (config.identifyIntervalMs <= 0)
    config.identifyIntervalMs = 2000;
  config.enrollCooldownMs =
      ConfigService::getInt("identity.enroll_cooldown_ms");
  if (config.enrollCooldownMs <= 0)
    config.enrollCooldownMs = 600000;
  config.bestShotMs = ConfigService::getInt("identity.best_shot_ms");
  if (config.bestShotMs <= 0)
    config.bestShotMs = 10000;
  config.improveMargin = ConfigService::getDouble("identity.improve_margin");
  if (config.improveMargin <= 0)
    config.improveMargin = 0.15;
  config.target = ConfigService::getString("identity.target");
  config.rpcSecret = ConfigService::getString("identity.rpc_secret");
  return config;
}
