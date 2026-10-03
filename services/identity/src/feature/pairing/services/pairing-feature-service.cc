#include "pairing-feature-service.hxx"

#include <cert/cert-service.hxx>
#include <config/config-service.hxx>
#include <config/identity-config.hxx>
#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>

#include <mutex>

ResponsePairingDto PairingFeatureService::pair(const PairingRequestInput& input) const
{
  {
    static std::mutex pairingMutex;
    const std::scoped_lock lock(pairingMutex);
    if (!CertService::verifyPairingCode(input.code))
      throw ResponseException(IdentityErrors::InvalidPairingCode);
    if (ConfigService::getBool("pairing.paired")) {
      if (ConfigService::getString("pairing.owner_device") != input.deviceHash)
        throw ResponseException(IdentityErrors::ServerAlreadyPaired);
    }
    else {
      ConfigService::setString("pairing.owner_device", input.deviceHash);
      ConfigService::setBool("pairing.paired", true);
    }
  }

  ResponsePairingDto result;
  result.instanceId = CertService::instanceId();
  result.caFingerprint = CertService::caFingerprint();
  result.serverFingerprint = CertService::serverFingerprint();
  result.caPem = CertService::caPem();
  result.scheme = "https";
  result.port = IdentityConfig::resolveAnnouncedPort();
  return result;
}

drogon::Task<ResponsePairingStatusDto> PairingFeatureService::status() const
{
  ResponsePairingStatusDto result;
  result.paired = ConfigService::getBool("pairing.paired");
  result.hasOwner = co_await userRepository_.hasOwner();
  co_return result;
}
