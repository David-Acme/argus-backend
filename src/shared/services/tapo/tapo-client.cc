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

TapoResult TapoClient::tryCandidate(const TapoCredentialCandidate& candidate)
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

  if (config_.candidates.empty())
    return TapoResult::failure("no credentials configured");

  TapoResult last = TapoResult::failure("no credential attempted");
  for (const auto& candidate : config_.candidates) {
    last = tryCandidate(candidate);
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
  return transport_->request(request);
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

Json::Value TapoClient::state() const
{
  const std::lock_guard<std::mutex> guard(mutex_);
  Json::Value value(Json::objectValue);
  value["host"] = config_.host;
  value["port"] = config_.port;
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
