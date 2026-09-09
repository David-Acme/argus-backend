#include "tapo-client.hxx"

#include <algorithm>
#include <drogon/drogon.h>
#include <shared/services/tapo/legacy-stok-transport.hxx>
#include <shared/services/tapo/secure-passthrough-transport.hxx>
#include <utility>

namespace
{

TapoCredentials credentialsOf(const TapoClientConfig& config,
                              const TapoCredentialCandidate& candidate)
{
  return {.host = config.host,
          .port = config.port,
          .username = candidate.username,
          .password = candidate.password,
          .connectTimeoutMs = config.connectTimeoutMs,
          .requestTimeoutMs = config.requestTimeoutMs};
}

} // namespace

TapoClient::TapoClient(TapoClientConfig config) : config_(std::move(config)) {}

std::unique_ptr<ITapoTransport>
TapoClient::makeTransport(TapoTransportKind kind, const TapoCredentialCandidate& candidate)
{
  auto credentials = credentialsOf(config_, candidate);
  if (kind == TapoTransportKind::LegacyStok)
    return std::make_unique<LegacyStokTransport>(std::move(credentials));
  return std::make_unique<SecurePassthroughTransport>(std::move(credentials));
}

TapoResult TapoClient::tryCandidate(
    const TapoCredentialCandidate& candidate,
    const std::optional<TapoTransportKind> excludedKind)
{
  std::vector<TapoTransportKind> order;
  switch (config_.transport) {
    case TapoTransportPreference::SecurePassthrough:
      order = {TapoTransportKind::SecurePassthrough};
      break;
    case TapoTransportPreference::LegacyStok:
      order = {TapoTransportKind::LegacyStok};
      break;
    case TapoTransportPreference::Auto:
      order = {TapoTransportKind::SecurePassthrough, TapoTransportKind::LegacyStok};
      break;
  }

  TapoResult last = TapoResult::failure("no transport attempted");
  for (const auto kind : order) {
    if (excludedKind && kind == *excludedKind)
      continue;
    auto transport = makeTransport(kind, candidate);
    for (int attempt = 0; attempt < std::max(1, config_.loginAttempts); ++attempt) {
      last = transport->login();
      if (last.ok) {
        transport_ = std::move(transport);
        credentialLabel_ = candidate.label;
        return last;
      }
    }
    LOG_DEBUG << "tapo: transport " << tapoTransportKindToString(kind)
              << " rejected credential '" << candidate.label << "': " << last.error;
  }
  return last;
}

TapoResult TapoClient::connect()
{
  const std::lock_guard<std::mutex> guard(mutex_);
  return connectLocked(std::nullopt);
}

TapoResult TapoClient::connectLocked(
    const std::optional<TapoTransportKind> excludedKind)
{
  transport_.reset();
  credentialLabel_.clear();

  if (config_.candidates.empty())
    return TapoResult::failure("no credentials configured");

  TapoResult last = TapoResult::failure("no credential attempted");
  for (const auto& candidate : config_.candidates) {
    last = tryCandidate(candidate, excludedKind);
    if (last.ok) {
      LOG_INFO << "tapo: connected to " << config_.host << " using credential '"
               << candidate.label << "' over "
               << tapoTransportKindToString(transport_->kind());
      return last;
    }
  }
  transport_.reset();
  credentialLabel_.clear();
  return last;
}

TapoResult TapoClient::invoke(const Json::Value& request)
{
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!transport_)
    return TapoResult::failure("client not connected");

  const auto activeKind = transport_->kind();
  const auto result = transport_->request(request);
  if (result.ok || !isRecoverableFailure(result))
    return result;

  LOG_WARN << "tapo: " << tapoTransportKindToString(activeKind)
           << " request failed (" << result.error
           << "); attempting transport fallback";

  const auto reconnect = connectLocked(
      config_.transport == TapoTransportPreference::Auto
          ? std::optional<TapoTransportKind>(activeKind)
          : std::nullopt);
  if (!reconnect.ok) {
    return TapoResult::failure(
        result.error + "; fallback connection failed: " + reconnect.error,
        result.errorCode);
  }

  if (isSafeToRetry(request)) {
    const auto retry = transport_->request(request);
    if (retry.ok)
      return retry;
    LOG_WARN << "tapo: fallback transport request failed: " << retry.error;
  }

  return TapoResult::failure(
      result.error + "; fallback connected over " +
          tapoTransportKindToString(transport_->kind()) +
          "; repeat the operation",
      result.errorCode);
}

TapoResult TapoClient::batch(const std::vector<Json::Value>& requests)
{
  if (requests.empty())
    return TapoResult::failure("empty batch");

  Json::Value payload(Json::objectValue);
  payload["method"] = "multipleRequest";
  Json::Value list(Json::arrayValue);
  for (const auto& request : requests)
    list.append(request);
  payload["params"]["requests"] = list;

  return invoke(payload);
}

bool TapoClient::isConnected() const
{
  const std::lock_guard<std::mutex> guard(mutex_);
  return transport_ && transport_->isAuthenticated();
}

bool TapoClient::isRecoverableFailure(const TapoResult& result)
{
  if (result.errorCode == -1 || result.errorCode == -40401)
    return true;
  if (result.errorCode != 0)
    return false;

  return result.error.find("cannot connect") != std::string::npos ||
         result.error.find("TLS") != std::string::npos ||
         result.error.find("tls") != std::string::npos ||
         result.error.find("timeout") != std::string::npos ||
         result.error.find("connection") != std::string::npos ||
         result.error.find("transport") != std::string::npos ||
         result.error.find("malformed") != std::string::npos ||
         result.error.find("response without payload") != std::string::npos ||
         result.error == "login rejected";
}

bool TapoClient::isSafeToRetry(const Json::Value& request)
{
  const auto method = request.get("method", "").asString();
  if (method == "get")
    return true;
  if (method != "multipleRequest")
    return false;

  const auto& requests = request["params"]["requests"];
  if (!requests.isArray() || requests.empty())
    return false;
  for (const auto& item : requests) {
    const auto nestedMethod = item.get("method", "").asString();
    if (nestedMethod.rfind("get", 0) != 0 &&
        nestedMethod != "searchDetectionList")
      return false;
  }
  return true;
}

Json::Value TapoClient::state() const
{
  const std::lock_guard<std::mutex> guard(mutex_);
  Json::Value value(Json::objectValue);
  value["host"] = config_.host;
  value["port"] = config_.port;
  value["connected"] = transport_ && transport_->isAuthenticated();
  value["credential"] = credentialLabel_;
  if (transport_) {
    const Json::Value transportState = transport_->state();
    for (const auto& key : transportState.getMemberNames())
      value[key] = transportState[key];
  }
  return value;
}

const std::string& TapoClient::credentialLabel() const
{
  return credentialLabel_;
}
