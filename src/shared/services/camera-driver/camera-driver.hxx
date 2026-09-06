#pragma once

#include <json/value.h>
#include <memory>
#include <optional>
#include <shared/schemas/camera/camera-schema.hxx>
#include <string>
#include <vector>

/** What a camera integration can be asked to do. Blocking by nature. */
struct DriverResult
{
  bool ok{false};
  std::string error;
  Json::Value data;

  static DriverResult failure(const std::string& message)
  {
    return {.ok = false, .error = message, .data = Json::Value()};
  }
};

struct DriverMoveInput
{
  std::optional<int64_t> x;
  std::optional<int64_t> y;
  /** Tapo protocol direction in degrees; wins over x/y when present. */
  std::optional<int64_t> angle;
};

struct DriverPresetInput
{
  std::string action;
  std::string id;
  std::string name;
};

struct DriverSettingsInput
{
  std::optional<bool> privacy;
  std::optional<bool> led;
  std::optional<std::string> dayNight;
  std::optional<bool> motion;
  std::optional<int> motionSensitivity;
  std::optional<bool> autoTrack;
  std::optional<bool> alarm;
  std::optional<int> alarmVolume;
};

struct DriverSpeakInput
{
  /** 16-bit PCM mono, already synthesized. */
  std::vector<int16_t> samples;
  int sampleRate{16000};
};

/** Everything the control layer knows about a camera, whoever makes it. */
class ICameraDriver
{
public:
  virtual ~ICameraDriver() = default;

  /** Feature flags, so the UI can hide what a model cannot do. */
  virtual Json::Value capabilities() const = 0;

  virtual DriverResult status() = 0;
  virtual DriverResult presets() = 0;
  virtual DriverResult move(const DriverMoveInput& input) = 0;
  virtual DriverResult preset(const DriverPresetInput& input) = 0;
  virtual DriverResult settings(const DriverSettingsInput& input) = 0;
  virtual DriverResult speak(const DriverSpeakInput& input) = 0;
};

/** Picks and caches the driver a camera row asks for. */
class CameraDriverRegistry
{
public:
  static CameraDriverRegistry& instance();

  std::shared_ptr<ICameraDriver> driverFor(const CameraSchema& camera);
  void forget(int64_t cameraId);
};

/** Test hook (same pattern as VoiceSessionTestAccess): seeds a stub driver
 * for a camera row in unit tests. */
struct CameraDriverTestAccess
{
  static void install(int64_t cameraId,
                      const std::shared_ptr<ICameraDriver>& driver);
};
