#include "pairing-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/pairing/dtos/pairing-dto.hxx>
#include <feature/api/pairing/dtos/response-pairing-dto.hxx>
#include <shared/services/cert/cert-service.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
PairingController::pair(drogon::HttpRequestPtr req)
{
  const auto body = PairingDto::fromJson(*req->getJsonObject());

  if (ConfigService::getBool("pairing.paired"))
    co_return AppConfig::get409Response("Server already paired");

  if (!CertService::verifyPairingCode(body.code))
    co_return AppConfig::get403Response("Invalid pairing code");

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
