#pragma once

#include <shared/services/onvif/onvif-soap.hxx>
#include <shared/services/onvif/onvif-transport.hxx>
#include <shared/services/onvif/onvif-types.hxx>
#include <shared/services/tapo/tapo-crypto.hxx>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct OnvifClientConfig
{
  OnvifEndpoint endpoint;
  std::shared_ptr<IOnvifTransport> transport;
  std::function<int64_t()> clock;
};

class OnvifClient
{
public:
  explicit OnvifClient(OnvifClientConfig config);

  OnvifOutcome<OnvifDevice> connect();
  [[nodiscard]] const std::optional<OnvifDevice>& device() const;

  OnvifOutcome<std::vector<OnvifPreset>> presets();
  OnvifOutcome<std::string> savePreset(const std::string& name);
  OnvifOutcome<bool> gotoPreset(const std::string& token);
  OnvifOutcome<bool> removePreset(const std::string& token);
  OnvifOutcome<bool> continuousMove(const OnvifVector& velocity);
  OnvifOutcome<bool> relativeMove(const OnvifVector& translation);
  OnvifOutcome<bool> stop();

private:
  struct Call
  {
    OnvifService service{OnvifService::Device};
    std::string operation;
    OnvifBodyWriter body;
    bool secured{true};
  };

  struct Settled
  {
    bool retry{false};
    OnvifOutcome<OnvifReply> outcome;
  };

  static Call presetsCall(const std::string& profile);
  OnvifOutcome<OnvifHttpRequest> prepared(const Call& request);
  Settled settled(const OnvifHttpResponse& response, int attempt);
  OnvifOutcome<OnvifReply> call(const Call& request);
  OnvifOutcome<OnvifReply> ptzCall(const Call& request);
  void forget();
  [[nodiscard]] std::string pathFor(OnvifService service) const;
  [[nodiscard]] int64_t now() const;
  [[nodiscard]] std::optional<OnvifSecurity> securityFor(const Call& request) const;
  std::string authorizationFor(const std::string& path);

  OnvifClientConfig config_;
  OnvifServices services_;
  std::optional<OnvifDevice> device_;
  int64_t clockOffsetSeconds_{0};
  std::optional<tapo_crypto::DigestChallenge> challenge_;
  uint32_t nonceCount_{0};
};
