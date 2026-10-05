#include "pairing-feature-service.hxx"

#include <cert/cert-service.hxx>
#include <config/config-service.hxx>
#include <config/identity-config.hxx>
#include <errors/response-exception.hxx>
#include <feature/pairing/infra/pairing-code.hxx>
#include <identity/identity-errors.hxx>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <mutex>

namespace
{
struct PairingVerdict
{
  bool proven{false};
  std::string serverProof;
};

PairingVerdict settle(const PairingRequestInput& input)
{
  static std::mutex pairingMutex;
  const std::scoped_lock lock(pairingMutex);
  const PairingCodeStore store(PairingCodeStore::defaultPath());
  PairingVerdict verdict;
  verdict.proven = input.proof.empty()
                       ? store.verifyCode(input.code)
                       : store.verifyProof({.nonce = input.nonce, .proof = input.proof});
  if (!verdict.proven)
    return verdict;
  if (!input.proof.empty())
    verdict.serverProof = store.serverProof(
        {.nonce = input.nonce, .caFingerprint = CertService::caFingerprint()});
  if (!ConfigService::getBool("pairing.paired")) {
    ConfigService::setString("pairing.owner_device", input.deviceHash);
    ConfigService::setBool("pairing.paired", true);
    if (store.rotate())
      LOG_INFO << "Pairing: the first device is paired; the pairing code was "
                  "replaced and the new one is in the certificate directory";
  }
  return verdict;
}
}

drogon::Task<ResponsePairingDto>
PairingFeatureService::pair(PairingRequestInput input) const
{
  const auto verdict = co_await BlockingTask<PairingVerdict>(
      [request = std::move(input)] { return settle(request); });
  if (!verdict.proven)
    throw ResponseException(IdentityErrors::InvalidPairingCode);

  ResponsePairingDto result;
  result.instanceId = CertService::instanceId();
  result.caFingerprint = CertService::caFingerprint();
  result.serverFingerprint = CertService::serverFingerprint();
  result.caPem = CertService::caPem();
  result.scheme = "https";
  result.port = IdentityConfig::resolveAnnouncedPort();
  result.serverProof = verdict.serverProof;
  co_return result;
}

drogon::Task<ResponsePairingStatusDto> PairingFeatureService::status() const
{
  ResponsePairingStatusDto result;
  result.paired = ConfigService::getBool("pairing.paired");
  result.hasOwner = co_await userRepository_.hasOwner();
  co_return result;
}
