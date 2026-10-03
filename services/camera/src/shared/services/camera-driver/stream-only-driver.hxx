#pragma once

#include <shared/services/camera-driver/camera-driver.hxx>

class StreamOnlyDriver final : public ICameraDriver
{
public:
  explicit StreamOnlyDriver(const CameraSchema& camera);

  [[nodiscard]] Json::Value capabilities() const override;
  DriverResult status() override;
  DriverResult presets() override;
  DriverResult move(const DriverMoveInput& input) override;
  DriverResult preset(const DriverPresetInput& input) override;
  DriverResult settings(const DriverSettingsInput& input) override;
  DriverResult speak(const DriverSpeakInput& input) override;

private:
  [[nodiscard]] DriverResult unsupported() const;

  CameraSchema camera_;
};
