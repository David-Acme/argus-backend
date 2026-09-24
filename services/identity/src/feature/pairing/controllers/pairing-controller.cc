#include "pairing-controller.hxx"

#include <errors/response-exception.hxx>
#include <feature/pairing/dtos/pairing-dto.hxx>
#include <feature/pairing/dtos/response-pairing-dto.hxx>
#include <http/api-response.hxx>
#include <identity/identity-errors.hxx>
#include <cert/cert-service.hxx>
#include <config/config-service.hxx>

drogon::Task<drogon::HttpResponsePtr>
PairingController::pair(drogon::HttpRequestPtr req)
{
  const auto body = PairingDto::fromJson(*req->getJsonObject());

  if (ConfigService::getBool("pairing.paired"))
    throw ResponseException(IdentityErrors::ServerAlreadyPaired);

  if (!CertService::verifyPairingCode(body.code))
    throw ResponseException(IdentityErrors::InvalidPairingCode);

  ConfigService::setBool("pairing.paired", true);

  const int port = ConfigService::getInt("mdns.port");

  ResponsePairingDto result;
  result.instanceId = CertService::instanceId();
  result.caFingerprint = CertService::caFingerprint();
  result.serverFingerprint = CertService::serverFingerprint();
  result.caPem = CertService::caPem();
  result.scheme = "https";
  result.port = port;

  co_return ApiResponse::ok(result.toJson());
}
