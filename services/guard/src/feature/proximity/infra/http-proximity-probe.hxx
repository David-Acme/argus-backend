#pragma once

#include <feature/proximity/services/proximity-ports.hxx>

#include <string>

struct HttpProximityProbeConfig
{
  std::string bleEntity{"argus_ble"};
  std::string homeAssistantToken;
  double timeoutSeconds{1.5};
};

class HttpProximityProbe final : public ProximityProbe
{
public:
  explicit HttpProximityProbe(HttpProximityProbeConfig config);

  [[nodiscard]] drogon::Task<std::optional<std::vector<BleHeard>>>
  readBle(ProximityReceiverRow receiver) const override;

  [[nodiscard]] drogon::Task<std::optional<bool>> readRadar(ProximitySensorRow sensor) const override;

private:
  struct Fetch
  {
    std::string host;
    int64_t port{80};
    std::string path;
    bool bearer{false};
  };

  [[nodiscard]] drogon::Task<std::optional<Json::Value>> fetch(Fetch request) const;

  HttpProximityProbeConfig config_;
};
