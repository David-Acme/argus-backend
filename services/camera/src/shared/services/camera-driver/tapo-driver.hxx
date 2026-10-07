#pragma once

#include <memory>
#include <mutex>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/tapo/tapo-api.hxx>

class TapoTalkClient;

std::string tapoTalkUsername();

class TapoDriver final : public ICameraDriver
{
public:
  explicit TapoDriver(const CameraSchema& camera);
  ~TapoDriver() override;

  static DriverResult probe(const CameraSchema& camera);

  Json::Value capabilities() const override;
  DriverResult status() override;
  DriverResult presets() override;
  DriverResult move(const DriverMoveInput& input) override;
  DriverResult preset(const DriverPresetInput& input) override;
  DriverResult settings(const DriverSettingsInput& input) override;
  DriverResult speak(const DriverSpeakInput& input) override;
  TalkLineOpen talkLine() override;
  [[nodiscard]] Json::Value controlStatus() const override;

private:
  DriverResult ensureConnected();

  CameraSchema camera_;
  std::unique_ptr<TapoApi> api_;
  std::shared_ptr<TapoTalkClient> talkClient_;
  std::shared_ptr<std::timed_mutex> lineMutex_;
};
