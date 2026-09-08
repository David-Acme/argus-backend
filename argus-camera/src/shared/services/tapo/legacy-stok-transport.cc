#include "legacy-stok-transport.hxx"

#include <drogon/drogon.h>
#include <shared/services/tapo/tapo-crypto.hxx>
#include <shared/services/tapo/tapo-http.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <utility>

namespace
{

constexpr int kTokenExpiredCode = -40401;
constexpr int kGenericFailureCode = -1;

TapoEndpoint endpointOf(const TapoCredentials& credentials)
{
  return {.host = credentials.host,
          .port = credentials.port,
          .tls = true,
          .connectTimeoutMs = credentials.connectTimeoutMs,
          .ioTimeoutMs = credentials.requestTimeoutMs};
}

std::vector<TapoHttpHeader> commonHeaders(const std::string& host)
{
  return {{"Content-Type", "application/json"},
          {"Accept", "application/json"},
          {"User-Agent", "Tapo CameraClient Android"},
          {"Referer", "https://" + host},
          {"Connection", "close"},
          {"requestByApp", "true"}};
}

int errorCodeOf(const Json::Value& response)
{
  if (response.isMember("error_code"))
    return response["error_code"].asInt();
  return 0;
}

} // namespace

LegacyStokTransport::LegacyStokTransport(TapoCredentials credentials)
    : credentials_(std::move(credentials))
{
}

TapoTransportKind LegacyStokTransport::kind() const
{
  return TapoTransportKind::LegacyStok;
}

bool LegacyStokTransport::isAuthenticated() const
{
  return authenticated_;
}

Json::Value LegacyStokTransport::state() const
{
  Json::Value value(Json::objectValue);
  value["transport"] = tapoTransportKindToString(kind());
  value["hashAlgorithm"] = tapoHashAlgorithmToString(TapoHashAlgorithm::Md5);
  value["username"] = credentials_.username;
  value["authenticated"] = authenticated_;
  return value;
}

TapoResult LegacyStokTransport::login()
{
  authenticated_ = false;

  Json::Value payload(Json::objectValue);
  payload["method"] = "login";
  payload["params"]["hashed"] = true;
  payload["params"]["password"] = tapo_crypto::md5Hex(credentials_.password);
  payload["params"]["username"] = credentials_.username;

  TapoHttpRequest request;
  request.endpoint = endpointOf(credentials_);
  request.method = "POST";
  request.path = "/";
  request.headers = commonHeaders(credentials_.host);
  request.body = json_util::toString(payload);

  const auto response = TapoHttp::send(request);
  if (!response.ok)
    return TapoResult::failure(response.error.empty() ? "transport error"
                                                      : response.error);

  const Json::Value parsed = json_util::fromString(response.body);
  if (parsed.isNull())
    return TapoResult::failure("malformed response body");
  if (!parsed["result"].isMember("stok"))
    return TapoResult::failure("legacy login rejected", errorCodeOf(parsed));

  stok_ = parsed["result"]["stok"].asString();
  authenticated_ = true;

  LOG_INFO << "tapo: legacy stok session established with " << credentials_.host;
  return TapoResult::success(parsed["result"]);
}

TapoResult LegacyStokTransport::send(const Json::Value& payload)
{
  TapoHttpRequest request;
  request.endpoint = endpointOf(credentials_);
  request.method = "POST";
  request.path = "/stok=" + stok_ + "/ds";
  request.headers = commonHeaders(credentials_.host);
  request.body = json_util::toString(payload);

  const auto response = TapoHttp::send(request);
  if (!response.ok)
    return TapoResult::failure(response.error.empty() ? "transport error"
                                                      : response.error);

  const Json::Value parsed = json_util::fromString(response.body);
  if (parsed.isNull())
    return TapoResult::failure("malformed response body");

  const int code = errorCodeOf(parsed);
  if (code != 0) {
    TapoResult result = TapoResult::failure("device returned error", code);
    result.data = parsed;
    return result;
  }
  return TapoResult::success(parsed);
}

TapoResult LegacyStokTransport::request(const Json::Value& payload)
{
  if (!authenticated_) {
    const auto session = login();
    if (!session.ok)
      return session;
  }

  auto result = send(payload);
  if (result.ok || (result.errorCode != kTokenExpiredCode &&
                    result.errorCode != kGenericFailureCode))
    return result;

  LOG_DEBUG << "tapo: legacy session expired, re-authenticating";
  authenticated_ = false;
  const auto session = login();
  if (!session.ok)
    return session;
  return send(payload);
}
