#pragma once

#include <json/value.h>
#include <memory>
#include <optional>
#include <shared/schemas/camera/camera-schema.hxx>
#include <span>
#include <string>
#include <vector>

struct DriverResult
{
  bool ok{false};
  std::string error;
  Json::Value data;
  bool attempted{false};

  static DriverResult failure(const std::string& message)
  {
    return {.ok = false, .error = message, .data = Json::Value()};
  }
};

struct DriverMoveInput
{
  std::optional<int64_t> x;
  std::optional<int64_t> y;
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
  std::optional<bool> sounding;
};

struct DriverSpeakInput
{
  std::vector<int16_t> samples;
  int sampleRate{16000};
};

class ICameraTalkLine
{
public:
  virtual ~ICameraTalkLine() = default;

  virtual DriverResult open() = 0;
  virtual DriverResult write(std::span<const int16_t> pcm8k) = 0;
  virtual void close() = 0;
};

struct TalkLineOpen
{
  std::unique_ptr<ICameraTalkLine> line;
  std::string error;
};

class ICameraDriver
{
public:
  virtual ~ICameraDriver() = default;

  virtual Json::Value capabilities() const = 0;

  virtual DriverResult status() = 0;
  virtual DriverResult presets() = 0;
  virtual DriverResult move(const DriverMoveInput& input) = 0;
  virtual DriverResult preset(const DriverPresetInput& input) = 0;
  virtual DriverResult settings(const DriverSettingsInput& input) = 0;
  virtual DriverResult speak(const DriverSpeakInput& input) = 0;
  virtual TalkLineOpen talkLine();
};

class CameraDriverRegistry
{
public:
  static CameraDriverRegistry& instance();

  std::shared_ptr<ICameraDriver> driverFor(const CameraSchema& camera);
  void forget(int64_t cameraId);
};

struct CameraDriverTestAccess
{
  static void install(int64_t cameraId,
                      const std::shared_ptr<ICameraDriver>& driver);
};
