#pragma once

#include <memory>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/tapo/tapo-api.hxx>

/** TP-Link Tapo, over the local HTTPS control port plus the 8800 talk channel. */
class TapoDriver final : public ICameraDriver
{
public:
  explicit TapoDriver(const CameraSchema& camera);

  Json::Value capabilities() const override;
  DriverResult status() override;
  DriverResult presets() override;
  DriverResult move(const DriverMoveInput& input) override;
  DriverResult preset(const DriverPresetInput& input) override;
  DriverResult settings(const DriverSettingsInput& input) override;
  DriverResult speak(const DriverSpeakInput& input) override;

private:
  DriverResult ensureConnected();

  CameraSchema camera_;
  std::unique_ptr<TapoApi> api_;
};
