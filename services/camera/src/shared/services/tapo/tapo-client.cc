#include "tapo-client.hxx"

#include <algorithm>
#include <drogon/drogon.h>
#include <shared/services/tapo/legacy-stok-transport.hxx>
#include <shared/services/tapo/secure-passthrough-transport.hxx>
#include <shared/services/tapo/tapo-control-hub.hxx>
#include <utility>

namespace
{

constexpr int64_t kMillisPerSecond = 1000;

TapoCredentials credentialsOf(const TapoClientConfig& config,
                              const TapoCredentialCandidate& candidate)
{
  return {.host = config.host,
          .port = config.port,
          .username = candidate.username,
          .password = candidate.password,
          .connectTimeoutMs = config.connectTimeoutMs,
          .requestTimeoutMs = config.requestTimeoutMs,
          .trust = config.trust};
}

std::unique_ptr<ITapoTransport> defaultTransport(const TapoTransportRequest& request)
{
  if (request.kind == TapoTransportKind::LegacyStok)
    return std::make_unique<LegacyStokTransport>(request.credentials);
  return std::make_unique<SecurePassthroughTransport>(request.credentials);
}

std::vector<TapoTransportKind> transportOrder(TapoTransportPreference preference)
{
  switch (preference) {
    case TapoTransportPreference::SecurePassthrough:
      return {TapoTransportKind::SecurePassthrough};
    case TapoTransportPreference::LegacyStok:
      return {TapoTransportKind::LegacyStok};
    case TapoTransportPreference::Auto:
      break;
  }
  return {TapoTransportKind::SecurePassthrough, TapoTransportKind::LegacyStok};
}

int secondsUntil(int64_t untilMs, int64_t nowMs)
{
  if (untilMs <= nowMs)
    return 0;
  return static_cast<int>((untilMs - nowMs + kMillisPerSecond - 1) / kMillisPerSecond);
}

}

TapoClient::TapoClient(TapoClientConfig config)
    : config_(std::move(config)), standing_(config_.candidates.size())
{
  if (!config_.nowMs)
    config_.nowMs = tapo_control::systemNowMs;
  if (!config_.transportFactory)
    config_.transportFactory = defaultTransport;

  if (config_.candidates.empty()) {
    publish(TapoControlState::NoCredentials, TapoResult::failure("no credentials configured"));
    return;
  }
  for (std::size_t index = 0; index < config_.candidates.size(); ++index) {
    const auto& candidate = config_.candidates[index];
    if (!config_.remembered.label.empty() && !candidate.memoryKey.empty() &&
        candidate.label == config_.remembered.label &&
        candidate.memoryKey == config_.remembered.key) {
      winner_ = index;
      persistedLabel_ = candidate.label;
      std::lock_guard<std::mutex> statusGuard(statusMutex_);
      published_.credential = candidate.label;
    }
  }
}

TapoResult TapoClient::attempt(const std::size_t index,
                               const std::optional<TapoTransportKind> excludedKind)
{
  const auto& candidate = config_.candidates[index];
  const bool secureSeen = config_.trust && config_.trust->secureSeen();
  TapoResult last = TapoResult::failure("no transport attempted");
  for (const auto kind : transportOrder(config_.transport)) {
    if (excludedKind && kind == *excludedKind)
      continue;
    if (secureSeen && kind == TapoTransportKind::LegacyStok) {
      last = TapoResult::failure(
          "legacy login refused: this camera already answered over secure passthrough");
      continue;
    }
    auto transport = config_.transportFactory(
        {.kind = kind, .credentials = credentialsOf(config_, candidate)});
    for (int tries = 0; tries < std::max(1, config_.loginAttempts); ++tries) {
      last = transport->login();
      if (last.ok) {
        if (kind == TapoTransportKind::SecurePassthrough && config_.trust)
          config_.trust->noteSecure();
        transport_ = std::move(transport);
        credentialLabel_ = candidate.label;
        return last;
      }
      if (last.kind != TapoFailureKind::Transport)
        break;
    }
    LOG_DEBUG << "tapo: transport " << tapoTransportKindToString(kind)
              << " rejected credential '" << candidate.label << "': " << last.error;
    if (last.kind == TapoFailureKind::LockedOut ||
        last.kind == TapoFailureKind::CredentialRefused ||
        last.kind == TapoFailureKind::Transport)
      return last;
  }
  return last;
}

std::vector<std::size_t> TapoClient::order() const
{
  if (winner_)
    return {*winner_};
  std::vector<std::size_t> open;
  for (std::size_t index = 0; index < standing_.size(); ++index) {
    if (standing_[index].verdict != Verdict::Refused)
      open.push_back(index);
  }
  if (!open.empty())
    return open;
  return {retryCursor_ % config_.candidates.size()};
}

TapoResult TapoClient::connect()
{
  const std::lock_guard<std::mutex> guard(mutex_);
  return connectLocked(std::nullopt);
}

TapoResult TapoClient::ensureConnected()
{
  const std::lock_guard<std::mutex> guard(mutex_);
  if (transport_ && transport_->isAuthenticated())
    return TapoResult::success(Json::Value());
  return connectLocked(std::nullopt);
}

TapoResult TapoClient::connectLocked(const std::optional<TapoTransportKind> excludedKind)
{
  transport_.reset();
  credentialLabel_.clear();

  if (config_.candidates.empty())
    return TapoResult::failure("no credentials configured");

  if (const auto shared = tapo_control_hub::holdFor(endpoint(), config_.nowMs());
      shared && shared->untilMs > blockedUntilMs_) {
    blockedUntilMs_ = shared->untilMs;
    blockedCode_ = shared->code;
    publish(shared->state, TapoResult::failure("camera locked login", shared->code));
  }
  if (config_.nowMs() < blockedUntilMs_)
    return blockedFailure();

  TapoResult last = TapoResult::failure("no credential attempted");
  for (const auto index : order()) {
    last = attempt(index, excludedKind);
    if (last.ok) {
      adopt(index);
      LOG_INFO << "tapo: connected to " << config_.host << " using credential '"
               << config_.candidates[index].label << "' over "
               << tapoTransportKindToString(transport_->kind());
      return last;
    }
    if (last.kind == TapoFailureKind::LockedOut) {
      enterLocked(last);
      return last;
    }
    if (last.kind == TapoFailureKind::CredentialRefused) {
      refuse(index);
      continue;
    }
    enterUnreachable(last);
    return last;
  }

  enterRefused();
  return blockedFailure();
}

void TapoClient::adopt(const std::size_t index)
{
  winner_ = index;
  standing_[index].verdict = Verdict::Won;
  standing_[index].refusals = 0;
  blockedUntilMs_ = 0;
  unreachableBackoffSeconds_ = 0;
  const auto& candidate = config_.candidates[index];
  if (candidate.label != persistedLabel_) {
    persistedLabel_ = candidate.label;
    if (config_.persistWinner)
      config_.persistWinner({.label = candidate.label, .key = candidate.memoryKey});
  }
  publish(TapoControlState::Ready, TapoResult());
}

void TapoClient::refuse(const std::size_t index)
{
  standing_[index].verdict = Verdict::Refused;
  ++standing_[index].refusals;
  if (winner_ && *winner_ == index)
    winner_.reset();
  retryCursor_ = index + 1;
}

void TapoClient::enterRefused()
{
  blockedUntilMs_ =
      config_.nowMs() + static_cast<int64_t>(config_.refusedRetrySeconds) * kMillisPerSecond;
  publish(TapoControlState::Refused, TapoResult::failure("credentials refused", blockedCode_));
}

void TapoClient::enterLocked(const TapoResult& result)
{
  blockedUntilMs_ =
      config_.nowMs() +
      static_cast<int64_t>(result.secLeft + config_.lockoutMarginSeconds) * kMillisPerSecond;
  blockedCode_ = result.errorCode;
  tapo_control_hub::hold(endpoint(), {.state = TapoControlState::LockedOut,
                                      .untilMs = blockedUntilMs_,
                                      .code = result.errorCode});
  publish(TapoControlState::LockedOut, result);
}

void TapoClient::enterUnreachable(const TapoResult& result)
{
  unreachableBackoffSeconds_ =
      unreachableBackoffSeconds_ == 0
          ? config_.unreachableBackoffMinSeconds
          : std::min(unreachableBackoffSeconds_ * 2, config_.unreachableBackoffMaxSeconds);
  blockedUntilMs_ =
      config_.nowMs() + static_cast<int64_t>(unreachableBackoffSeconds_) * kMillisPerSecond;
  blockedCode_ = result.errorCode;
  publish(TapoControlState::Unreachable, result);
}

TapoResult TapoClient::blockedFailure() const
{
  const int seconds = secondsUntil(blockedUntilMs_, config_.nowMs());
  const std::string wait = std::to_string(seconds) + " s";
  switch (state_) {
    case TapoControlState::LockedOut:
      return TapoResult::failure("camera locked login; retry in " + wait, blockedCode_)
          .as(TapoFailureKind::LockedOut, seconds);
    case TapoControlState::Refused:
      return TapoResult::failure("camera refused the saved credentials; next try in " + wait,
                                 blockedCode_)
          .as(TapoFailureKind::CredentialRefused, seconds);
    case TapoControlState::Unreachable:
      return TapoResult::failure("camera control port unreachable; next try in " + wait,
                                 blockedCode_)
          .as(TapoFailureKind::Transport, seconds);
    case TapoControlState::Idle:
    case TapoControlState::Ready:
    case TapoControlState::NoCredentials:
    case TapoControlState::NotApplicable:
      break;
  }
  return TapoResult::failure("camera control is paused");
}

std::string TapoClient::refusedLabels() const
{
  std::string labels;
  for (std::size_t index = 0; index < standing_.size(); ++index) {
    if (standing_[index].verdict != Verdict::Refused)
      continue;
    if (!labels.empty())
      labels += ", ";
    labels += config_.candidates[index].label;
  }
  return labels;
}

void TapoClient::publish(const TapoControlState state, const TapoResult& cause)
{
  const bool changed = state_ != state;
  state_ = state;

  TapoControlStatus next;
  next.state = state;
  next.credential = winner_ ? config_.candidates[*winner_].label : std::string();
  next.code = cause.errorCode;
  switch (state) {
    case TapoControlState::LockedOut:
      next.retryAtMs = blockedUntilMs_;
      next.message = "The camera locked its control login after failed attempts";
      break;
    case TapoControlState::Refused:
      next.retryAtMs = blockedUntilMs_;
      next.message = "The camera refused the saved credentials (" + refusedLabels() + ")";
      break;
    case TapoControlState::Unreachable:
      next.retryAtMs = blockedUntilMs_;
      next.message = "The camera control port did not answer: " + cause.error;
      break;
    case TapoControlState::NoCredentials:
      next.message = "No camera account or cloud password is saved";
      break;
    case TapoControlState::Idle:
    case TapoControlState::Ready:
    case TapoControlState::NotApplicable:
      break;
  }
  {
    std::lock_guard<std::mutex> statusGuard(statusMutex_);
    published_ = next;
  }

  if (!changed)
    return;
  switch (state) {
    case TapoControlState::LockedOut:
      LOG_WARN << "tapo: " << config_.host << " locked control login, no attempt for "
               << secondsUntil(blockedUntilMs_, config_.nowMs()) << " s (camera code "
               << cause.errorCode << ")";
      break;
    case TapoControlState::Refused:
      LOG_WARN << "tapo: " << config_.host << " refused credentials [" << refusedLabels()
               << "], next single try in " << secondsUntil(blockedUntilMs_, config_.nowMs())
               << " s";
      break;
    case TapoControlState::Unreachable:
      LOG_WARN << "tapo: " << config_.host << " control port did not answer ("
               << cause.error << "), next try in "
               << secondsUntil(blockedUntilMs_, config_.nowMs()) << " s";
      break;
    case TapoControlState::Idle:
    case TapoControlState::Ready:
    case TapoControlState::NoCredentials:
    case TapoControlState::NotApplicable:
      break;
  }
}

TapoResult TapoClient::invoke(const Json::Value& request)
{
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!transport_ || !transport_->isAuthenticated()) {
    const auto ready = connectLocked(std::nullopt);
    if (!ready.ok)
      return ready;
  }

  const auto activeKind = transport_->kind();
  const auto result = transport_->request(request);
  if (result.ok)
    return result;

  if (result.kind == TapoFailureKind::LockedOut) {
    transport_.reset();
    enterLocked(result);
    return result;
  }
  if (result.kind == TapoFailureKind::CredentialRefused) {
    if (winner_)
      refuse(*winner_);
    transport_.reset();
    enterRefused();
    return result;
  }
  if (!isRecoverableFailure(result))
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
               result.errorCode)
        .as(reconnect.kind, reconnect.secLeft);
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
  if (result.kind == TapoFailureKind::Transport)
    return true;
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
         result.error.find("response without payload") != std::string::npos;
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
  Json::Value value(Json::objectValue);
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    value["host"] = config_.host;
    value["port"] = config_.port;
    value["connected"] = transport_ && transport_->isAuthenticated();
    value["credential"] = credentialLabel_;
    if (transport_) {
      const Json::Value transportState = transport_->state();
      for (const auto& key : transportState.getMemberNames())
        value[key] = transportState[key];
    }
  }
  value["control"] = tapo_control::toJson(status(), config_.nowMs());
  return value;
}

std::string TapoClient::credentialLabel() const
{
  const std::lock_guard<std::mutex> guard(mutex_);
  return credentialLabel_;
}

TapoControlStatus TapoClient::status() const
{
  const std::lock_guard<std::mutex> statusGuard(statusMutex_);
  return published_;
}

std::string TapoClient::endpoint() const
{
  return config_.host + ":" + std::to_string(config_.port);
}
