#pragma once

#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/onvif/onvif-client.hxx>

#include <memory>
#include <mutex>
#include <optional>

struct OnvifDriverParts
{
  CameraSchema camera;
  std::shared_ptr<IOnvifTransport> transport;
  const CapabilityRecorder* recorder{nullptr};
};

class OnvifDriver final : public ICameraDriver
{
public:
  OnvifDriver(const CameraSchema& camera, const CapabilityRecorder& recorder);
  OnvifDriver(const CameraSchema& camera, std::shared_ptr<IOnvifTransport> transport);
  explicit OnvifDriver(OnvifDriverParts parts);

  static int portOf(const CameraSchema& camera);
  static OnvifEndpoint endpointOf(const CameraSchema& camera);

  [[nodiscard]] Json::Value capabilities() const override;
  Json::Value discoverCapabilities() override;
  DriverResult status() override;
  DriverResult presets() override;
  DriverResult move(const DriverMoveInput& input) override;
  DriverResult preset(const DriverPresetInput& input) override;
  DriverResult settings(const DriverSettingsInput& input) override;
  DriverResult speak(const DriverSpeakInput& input) override;

private:
  DriverResult connected();

  CameraSchema camera_;
  const CapabilityRecorder* recorder_;
  std::mutex callMutex_;
  OnvifClient client_;
  mutable std::mutex knownMutex_;
  std::optional<OnvifDevice> known_;
};
