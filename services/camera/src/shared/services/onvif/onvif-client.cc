#include <shared/services/onvif/onvif-client.hxx>

#include <shared/services/onvif/onvif-parse.hxx>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>

namespace
{
constexpr size_t kNonceBytes = 16;
constexpr size_t kClientNonceChars = 16;
constexpr const char* kNoPtz = "This camera has no pan and tilt over ONVIF";
constexpr double kHalfStep = 0.00005;
constexpr const char* kHoldTimeout = "PT2S";
constexpr int kAttempts = 2;
constexpr const char* kRefusedAccount = "The camera refused the ONVIF user name or password";
constexpr const char* kNoTransport = "This ONVIF client has no transport for that call";
constexpr const char* kSubscriptionLifetime = "PT60S";
constexpr const char* kPullTimeout = "PT10S";
constexpr const char* kPullLimit = "10";
constexpr double kPullSeconds = 15.0;
constexpr double kUnsubscribeSeconds = 2.0;
constexpr const char* kEventPropertiesAction =
    "http://www.onvif.org/ver10/events/wsdl/EventPortType/GetEventPropertiesRequest";
constexpr const char* kCreatePullPointAction =
    "http://www.onvif.org/ver10/events/wsdl/EventPortType/CreatePullPointSubscriptionRequest";
constexpr const char* kPullMessagesAction =
    "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest";
constexpr const char* kRenewAction = "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest";
constexpr const char* kUnsubscribeAction =
    "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest";

void appendText(pugi::xml_node& parent, const char* name, const std::string& value)
{
  parent.append_child(name).text().set(value.c_str());
}

std::string coordinate(double value)
{
  const double bounded = std::clamp(value, -1.0, 1.0);
  return std::format("{:.4f}", std::abs(bounded) < kHalfStep ? 0.0 : bounded);
}

void appendPanTilt(pugi::xml_node& parent, const char* name, const OnvifVector& vector)
{
  auto pan = parent.append_child(name).append_child("tt:PanTilt");
  pan.append_attribute("x") = coordinate(vector.x).c_str();
  pan.append_attribute("y") = coordinate(vector.y).c_str();
}

bool wantsDigest(std::string_view challenge)
{
  constexpr std::string_view kScheme = "digest";
  if (challenge.size() < kScheme.size())
    return false;
  return std::ranges::equal(challenge.substr(0, kScheme.size()), kScheme, [](char left, char right) {
    return std::tolower(static_cast<unsigned char>(left)) == right;
  });
}

std::string_view qopOf(std::string_view offered)
{
  size_t start = 0;
  while (start <= offered.size()) {
    const size_t comma = offered.find(',', start);
    std::string_view token =
        offered.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    while (!token.empty() && token.front() == ' ')
      token.remove_prefix(1);
    while (!token.empty() && token.back() == ' ')
      token.remove_suffix(1);
    if (token == "auth")
      return "auth";
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  return {};
}

std::string faultMessage(const OnvifReply& reply)
{
  std::string message = "The camera refused the ONVIF request";
  if (!reply.faultReason.empty())
    message += ": " + reply.faultReason;
  if (!reply.faultCodes.empty())
    message += " (" + reply.faultCodes.back() + ")";
  return message;
}

template <typename T, typename From>
OnvifOutcome<T> carried(const OnvifOutcome<From>& from)
{
  return OnvifOutcome<T>::failed(from.failure, from.error);
}

bool losesSession(OnvifFailure failure)
{
  return failure == OnvifFailure::Transport || failure == OnvifFailure::Unauthorized;
}

pugi::xml_node appendAddressed(pugi::xml_node& header, const char* name)
{
  auto node = header.append_child(name);
  node.append_attribute("xmlns:wsa") = onvif_soap::kAddressingNamespace;
  return node;
}

OnvifBodyWriter addressing(const char* action, const OnvifSubscription& subscription)
{
  return [action, address = subscription.address, references = subscription.references](pugi::xml_node& header) {
    appendAddressed(header, "wsa:Action").text().set(action);
    appendAddressed(header, "wsa:To").text().set(address.c_str());
    if (!references.empty())
      static_cast<void>(header.append_buffer(references.data(), references.size()));
  };
}
}

OnvifClient::OnvifClient(OnvifClientConfig config) : config_(std::move(config)) {}

const std::optional<OnvifDevice>& OnvifClient::device() const
{
  return device_;
}

int64_t OnvifClient::now() const
{
  if (config_.clock)
    return config_.clock();
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string OnvifClient::pathFor(OnvifService service) const
{
  switch (service) {
    case OnvifService::Device:
      return services_.device;
    case OnvifService::Media:
      return services_.media;
    case OnvifService::Ptz:
      return services_.ptz;
    case OnvifService::Events:
      return services_.events;
    case OnvifService::Notification:
      return {};
  }
  return {};
}

std::optional<OnvifSecurity> OnvifClient::securityFor(const Call& request) const
{
  if (!request.secured || config_.endpoint.username.empty())
    return std::nullopt;
  return OnvifSecurity{.username = config_.endpoint.username,
                       .password = config_.endpoint.password,
                       .nonce = tapo_crypto::randomBytes(kNonceBytes),
                       .created = onvif_soap::createdAt(now() + clockOffsetSeconds_)};
}

std::string OnvifClient::authorizationFor(const std::string& path)
{
  const tapo_crypto::DigestChallenge& challenge = challenge_.value_or(tapo_crypto::DigestChallenge{});
  return tapo_crypto::buildDigestHeader({.username = config_.endpoint.username,
                                         .password = config_.endpoint.password,
                                         .realm = challenge.realm,
                                         .nonce = challenge.nonce,
                                         .qop = std::string(qopOf(challenge.qop)),
                                         .opaque = challenge.opaque,
                                         .algorithm = challenge.algorithm,
                                         .method = "POST",
                                         .uri = path,
                                         .cnonce = tapo_crypto::randomHex(kClientNonceChars),
                                         .nonceCount = ++nonceCount_});
}

OnvifOutcome<OnvifHttpRequest> OnvifClient::prepared(const Call& request, const Route& route)
{
  const std::string path = route.path.empty() ? pathFor(request.service) : route.path;
  if (path.empty())
    return OnvifOutcome<OnvifHttpRequest>::failed(
        OnvifFailure::Unsupported,
        "The camera offers no ONVIF " + onvifServiceToString(request.service) + " service");
  return {.value = OnvifHttpRequest{
              .path = path,
              .action = route.action.empty() ? onvif_soap::actionOf(request.service, request.operation)
                                             : route.action,
              .body = onvif_soap::envelope({.service = request.service,
                                            .operation = request.operation,
                                            .body = request.body,
                                            .security = securityFor(request),
                                            .header = route.header}),
              .authorization = challenge_ ? authorizationFor(path) : std::string(),
              .timeoutSeconds = route.timeoutSeconds}};
}

OnvifClient::Settled OnvifClient::settled(const OnvifHttpResponse& response, int attempt)
{
  if (!response.error.empty())
    return {.retry = false, .outcome = OnvifOutcome<OnvifReply>::failed(OnvifFailure::Transport, response.error)};
  if (response.status == 401) {
    if (attempt == 0 && wantsDigest(response.challenge) && !config_.endpoint.username.empty()) {
      challenge_ = tapo_crypto::parseDigestChallenge(response.challenge);
      nonceCount_ = 0;
      return {.retry = true, .outcome = {}};
    }
    challenge_.reset();
    return {.retry = false, .outcome = OnvifOutcome<OnvifReply>::failed(OnvifFailure::Unauthorized, kRefusedAccount)};
  }
  auto answer = onvif_soap::reply(response.body);
  if (!answer)
    return {.retry = false,
            .outcome = OnvifOutcome<OnvifReply>::failed(
                OnvifFailure::Malformed,
                "The camera's ONVIF answer (HTTP " + std::to_string(response.status) + ") is not SOAP")};
  if (answer->fault)
    return {.retry = false,
            .outcome = OnvifOutcome<OnvifReply>::failed(onvif_soap::faultKind(*answer), faultMessage(*answer))};
  if (response.status < 200 || response.status >= 300)
    return {.retry = false,
            .outcome = OnvifOutcome<OnvifReply>::failed(
                OnvifFailure::Fault,
                "The camera answered the ONVIF request with HTTP " + std::to_string(response.status))};
  return {.retry = false, .outcome = {.value = std::move(*answer)}};
}

OnvifOutcome<OnvifReply> OnvifClient::call(const Call& request)
{
  if (!config_.transport)
    return OnvifOutcome<OnvifReply>::failed(OnvifFailure::Unsupported, kNoTransport);
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    auto http = prepared(request, Route{});
    if (!http.ok())
      return carried<OnvifReply>(http);
    auto verdict = settled(config_.transport->post(http.value), attempt);
    if (!verdict.retry)
      return std::move(verdict.outcome);
  }
  return OnvifOutcome<OnvifReply>::failed(OnvifFailure::Unauthorized, kRefusedAccount);
}

drogon::Task<OnvifOutcome<OnvifReply>> OnvifClient::callAsync(Call request, Route route)
{
  if (!config_.asyncTransport)
    co_return OnvifOutcome<OnvifReply>::failed(OnvifFailure::Unsupported, kNoTransport);
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    auto http = prepared(request, route);
    if (!http.ok())
      co_return carried<OnvifReply>(http);
    const OnvifHttpResponse response = co_await config_.asyncTransport->send(std::move(http.value));
    auto verdict = settled(response, attempt);
    if (!verdict.retry)
      co_return std::move(verdict.outcome);
  }
  co_return OnvifOutcome<OnvifReply>::failed(OnvifFailure::Unauthorized, kRefusedAccount);
}

void OnvifClient::forget()
{
  device_.reset();
  services_ = OnvifServices{};
  challenge_.reset();
  nonceCount_ = 0;
}

OnvifOutcome<OnvifReply> OnvifClient::ptzCall(const Call& request)
{
  if (!device_ || !device_->ptz)
    return OnvifOutcome<OnvifReply>::failed(OnvifFailure::Unsupported, kNoPtz);
  auto answer = call(request);
  if (losesSession(answer.failure))
    forget();
  return answer;
}

OnvifClient::Call OnvifClient::presetsCall(const std::string& profile)
{
  return {.service = OnvifService::Ptz,
          .operation = "GetPresets",
          .body = [profile](pugi::xml_node& operation) {
            appendText(operation, "tptz:ProfileToken", profile);
          },
          .secured = true};
}

OnvifOutcome<OnvifDevice> OnvifClient::connect()
{
  device_.reset();
  services_ = OnvifServices{};
  OnvifDevice device;
  const auto abandon = [this](const auto& outcome) {
    if (losesSession(outcome.failure))
      forget();
    return carried<OnvifDevice>(outcome);
  };

  const auto deviceClock = call({.service = OnvifService::Device,
                                 .operation = "GetSystemDateAndTime",
                                 .body = {},
                                 .secured = false});
  if (deviceClock.failure == OnvifFailure::Transport)
    return abandon(deviceClock);
  if (deviceClock.ok()) {
    if (const auto epoch = onvif_parse::deviceEpoch(deviceClock.value.response))
      clockOffsetSeconds_ = *epoch - now();
  }
  device.clockOffsetSeconds = clockOffsetSeconds_;

  const auto capabilities = call({.service = OnvifService::Device,
                                  .operation = "GetCapabilities",
                                  .body = [](pugi::xml_node& operation) {
                                    appendText(operation, "tds:Category", "All");
                                  },
                                  .secured = true});
  if (!capabilities.ok())
    return abandon(capabilities);
  services_ = onvif_parse::services(capabilities.value.response);
  device.services = services_;

  const auto information = call({.service = OnvifService::Device,
                                 .operation = "GetDeviceInformation",
                                 .body = {},
                                 .secured = true});
  if (!information.ok())
    return abandon(information);
  device.info = onvif_parse::deviceInfo(information.value.response);

  if (!services_.media.empty()) {
    const auto answer = call({.service = OnvifService::Media,
                              .operation = "GetProfiles",
                              .body = {},
                              .secured = true});
    if (losesSession(answer.failure))
      return abandon(answer);
    const auto profiles = answer.ok() ? onvif_parse::profiles(answer.value.response)
                                      : std::vector<OnvifProfile>{};
    const auto steerable = std::ranges::find_if(
        profiles, [](const OnvifProfile& profile) { return !profile.ptzConfiguration.empty(); });
    if (steerable != profiles.end())
      device.profileToken = steerable->token;
    else if (!profiles.empty())
      device.profileToken = profiles.front().token;
    device.ptz = !services_.ptz.empty() && steerable != profiles.end();
  }

  if (device.ptz) {
    const auto stored = call(presetsCall(device.profileToken));
    if (stored.failure == OnvifFailure::Transport)
      return abandon(stored);
    device.presets = stored.ok();
  }
  device_ = device;
  return {.value = std::move(device)};
}

OnvifOutcome<std::vector<OnvifPreset>> OnvifClient::presets()
{
  const auto answer = ptzCall(presetsCall(device_ ? device_->profileToken : std::string()));
  if (!answer.ok())
    return carried<std::vector<OnvifPreset>>(answer);
  return {.value = onvif_parse::presets(answer.value.response)};
}

OnvifOutcome<std::string> OnvifClient::savePreset(const std::string& name)
{
  const std::string profile = device_ ? device_->profileToken : std::string();
  const auto answer = ptzCall({.service = OnvifService::Ptz,
                               .operation = "SetPreset",
                               .body = [profile, name](pugi::xml_node& operation) {
                                 appendText(operation, "tptz:ProfileToken", profile);
                                 appendText(operation, "tptz:PresetName", name);
                               },
                               .secured = true});
  if (!answer.ok())
    return carried<std::string>(answer);
  std::string token = onvif_parse::presetToken(answer.value.response);
  if (token.empty())
    return OnvifOutcome<std::string>::failed(OnvifFailure::Malformed,
                                             "The camera saved the preset without naming its token");
  return {.value = std::move(token)};
}

OnvifOutcome<bool> OnvifClient::gotoPreset(const std::string& token)
{
  const std::string profile = device_ ? device_->profileToken : std::string();
  const auto answer = ptzCall({.service = OnvifService::Ptz,
                               .operation = "GotoPreset",
                               .body = [profile, token](pugi::xml_node& operation) {
                                 appendText(operation, "tptz:ProfileToken", profile);
                                 appendText(operation, "tptz:PresetToken", token);
                               },
                               .secured = true});
  if (!answer.ok())
    return carried<bool>(answer);
  return {.value = true};
}

OnvifOutcome<bool> OnvifClient::removePreset(const std::string& token)
{
  const std::string profile = device_ ? device_->profileToken : std::string();
  const auto answer = ptzCall({.service = OnvifService::Ptz,
                               .operation = "RemovePreset",
                               .body = [profile, token](pugi::xml_node& operation) {
                                 appendText(operation, "tptz:ProfileToken", profile);
                                 appendText(operation, "tptz:PresetToken", token);
                               },
                               .secured = true});
  if (!answer.ok())
    return carried<bool>(answer);
  return {.value = true};
}

OnvifOutcome<bool> OnvifClient::continuousMove(const OnvifVector& velocity)
{
  const std::string profile = device_ ? device_->profileToken : std::string();
  const auto answer = ptzCall({.service = OnvifService::Ptz,
                               .operation = "ContinuousMove",
                               .body = [profile, velocity](pugi::xml_node& operation) {
                                 appendText(operation, "tptz:ProfileToken", profile);
                                 appendPanTilt(operation, "tptz:Velocity", velocity);
                                 appendText(operation, "tptz:Timeout", kHoldTimeout);
                               },
                               .secured = true});
  if (!answer.ok())
    return carried<bool>(answer);
  return {.value = true};
}

OnvifOutcome<bool> OnvifClient::relativeMove(const OnvifVector& translation)
{
  const std::string profile = device_ ? device_->profileToken : std::string();
  const auto answer = ptzCall({.service = OnvifService::Ptz,
                               .operation = "RelativeMove",
                               .body = [profile, translation](pugi::xml_node& operation) {
                                 appendText(operation, "tptz:ProfileToken", profile);
                                 appendPanTilt(operation, "tptz:Translation", translation);
                               },
                               .secured = true});
  if (!answer.ok())
    return carried<bool>(answer);
  return {.value = true};
}

OnvifOutcome<bool> OnvifClient::stop()
{
  const std::string profile = device_ ? device_->profileToken : std::string();
  const auto answer = ptzCall({.service = OnvifService::Ptz,
                               .operation = "Stop",
                               .body = [profile](pugi::xml_node& operation) {
                                 appendText(operation, "tptz:ProfileToken", profile);
                                 appendText(operation, "tptz:PanTilt", "true");
                               },
                               .secured = true});
  if (!answer.ok())
    return carried<bool>(answer);
  return {.value = true};
}

drogon::Task<OnvifOutcome<bool>> OnvifClient::openEvents()
{
  services_ = OnvifServices{};
  const auto deviceClock = co_await callAsync({.service = OnvifService::Device,
                                               .operation = "GetSystemDateAndTime",
                                               .body = {},
                                               .secured = false},
                                              Route{});
  if (deviceClock.failure == OnvifFailure::Transport) {
    forget();
    co_return carried<bool>(deviceClock);
  }
  if (deviceClock.ok()) {
    if (const auto epoch = onvif_parse::deviceEpoch(deviceClock.value.response))
      clockOffsetSeconds_ = *epoch - now();
  }
  const auto capabilities = co_await callAsync({.service = OnvifService::Device,
                                                .operation = "GetCapabilities",
                                                .body = [](pugi::xml_node& operation) {
                                                  appendText(operation, "tds:Category", "All");
                                                },
                                                .secured = true},
                                               Route{});
  if (!capabilities.ok()) {
    if (losesSession(capabilities.failure))
      forget();
    co_return carried<bool>(capabilities);
  }
  services_ = onvif_parse::services(capabilities.value.response);
  if (services_.events.empty())
    co_return OnvifOutcome<bool>::failed(OnvifFailure::Unsupported, "The camera offers no ONVIF events service");
  const auto topics = co_await callAsync({.service = OnvifService::Events,
                                          .operation = "GetEventProperties",
                                          .body = {},
                                          .secured = true},
                                         {.path = {}, .action = kEventPropertiesAction, .header = {}, .timeoutSeconds = 0.0});
  if (losesSession(topics.failure)) {
    forget();
    co_return carried<bool>(topics);
  }
  if (topics.ok()) {
    if (const auto motion = onvif_parse::publishesMotion(topics.value.response); motion && !*motion)
      co_return OnvifOutcome<bool>::failed(OnvifFailure::Unsupported, "The camera publishes no ONVIF motion topic");
  }
  co_return {.value = true};
}

drogon::Task<OnvifOutcome<OnvifSubscription>> OnvifClient::subscribe()
{
  const auto answer = co_await callAsync({.service = OnvifService::Events,
                                          .operation = "CreatePullPointSubscription",
                                          .body = [](pugi::xml_node& operation) {
                                            appendText(operation, "tev:InitialTerminationTime", kSubscriptionLifetime);
                                          },
                                          .secured = true},
                                         {.path = {}, .action = kCreatePullPointAction, .header = {}, .timeoutSeconds = 0.0});
  if (!answer.ok()) {
    if (losesSession(answer.failure))
      forget();
    co_return carried<OnvifSubscription>(answer);
  }
  auto made = onvif_parse::subscription(answer.value.response);
  if (!made)
    co_return OnvifOutcome<OnvifSubscription>::failed(OnvifFailure::Malformed,
                                                      "The camera named no address for its event subscription");
  co_return {.value = std::move(*made)};
}

drogon::Task<OnvifOutcome<OnvifPull>> OnvifClient::pull(OnvifSubscription subscription)
{
  const auto answer = co_await callAsync({.service = OnvifService::Events,
                                          .operation = "PullMessages",
                                          .body = [](pugi::xml_node& operation) {
                                            appendText(operation, "tev:Timeout", kPullTimeout);
                                            appendText(operation, "tev:MessageLimit", kPullLimit);
                                          },
                                          .secured = true},
                                         {.path = subscription.path,
                                          .action = kPullMessagesAction,
                                          .header = addressing(kPullMessagesAction, subscription),
                                          .timeoutSeconds = kPullSeconds});
  if (!answer.ok()) {
    if (losesSession(answer.failure))
      forget();
    co_return carried<OnvifPull>(answer);
  }
  co_return {.value = onvif_parse::pulled(answer.value.response)};
}

drogon::Task<OnvifOutcome<int64_t>> OnvifClient::renew(OnvifSubscription subscription)
{
  const auto answer = co_await callAsync({.service = OnvifService::Notification,
                                          .operation = "Renew",
                                          .body = [](pugi::xml_node& operation) {
                                            appendText(operation, "wsnt:TerminationTime", kSubscriptionLifetime);
                                          },
                                          .secured = true},
                                         {.path = subscription.path,
                                          .action = kRenewAction,
                                          .header = addressing(kRenewAction, subscription),
                                          .timeoutSeconds = 0.0});
  if (!answer.ok()) {
    if (losesSession(answer.failure))
      forget();
    co_return carried<int64_t>(answer);
  }
  co_return {.value = onvif_parse::lifetimeOf(answer.value.response)};
}

drogon::Task<OnvifOutcome<bool>> OnvifClient::unsubscribe(OnvifSubscription subscription)
{
  const auto answer = co_await callAsync({.service = OnvifService::Notification,
                                          .operation = "Unsubscribe",
                                          .body = {},
                                          .secured = true},
                                         {.path = subscription.path,
                                          .action = kUnsubscribeAction,
                                          .header = addressing(kUnsubscribeAction, subscription),
                                          .timeoutSeconds = kUnsubscribeSeconds});
  if (!answer.ok())
    co_return carried<bool>(answer);
  co_return {.value = true};
}
