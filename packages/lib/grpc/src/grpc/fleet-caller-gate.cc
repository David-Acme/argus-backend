#include "fleet-caller-gate.hxx"

#include <algorithm>

namespace argus::client
{

namespace
{

bool listed(CallerSet allowed, std::string_view caller)
{
  return std::ranges::find(allowed, caller) != allowed.end();
}

bool presentedOnce(const ClientMetadata& metadata, std::string_view key)
{
  return metadata.count(grpc::string_ref(key.data(), key.size())) <= 1;
}

std::string presentedValue(const ClientMetadata& metadata, std::string_view key)
{
  const auto found = metadata.find(grpc::string_ref(key.data(), key.size()));
  if (found == metadata.end())
    return {};
  return {found->second.data(), found->second.size()};
}

}

std::optional<PresentedCredential> presentedCredential(const ClientMetadata& metadata)
{
  if (!presentedOnce(metadata, kCallerCredentialKey) ||
      !presentedOnce(metadata, kFleetSecretKey))
    return std::nullopt;
  return PresentedCredential{.credential = presentedValue(metadata, kCallerCredentialKey),
                             .fleetSecret = presentedValue(metadata, kFleetSecretKey)};
}

bool FleetCallerGate::pairedSecret(std::string_view secret)
{
  return !secret.empty() && secret.find(kUnpairedMarker) == std::string_view::npos;
}

FleetCallerGate::FleetCallerGate(FleetGateConfig config)
    : onFirstLegacy_(std::move(config.onFirstLegacy))
{
  for (auto& [service, secret] : config.callerPairs) {
    if (service.empty() || !pairedSecret(secret))
      continue;
    credentials_.push_back({.service = std::move(service), .secret = std::move(secret)});
  }
  for (auto& expected : config.expectedCallers) {
    const bool paired = std::ranges::any_of(
        credentials_, [&expected](const CallerCredential& credential) {
          return credential.service == expected;
        });
    if (!paired)
      unpaired_.push_back(std::move(expected));
  }
  if (pairedSecret(config.legacySecret))
    legacySecret_ = std::move(config.legacySecret);
}

bool FleetCallerGate::open() const
{
  return credentials_.empty() && legacySecret_.empty();
}

bool FleetCallerGate::acceptsLegacy() const
{
  return !legacySecret_.empty() && !unpaired_.empty();
}

std::size_t FleetCallerGate::pairedCount() const
{
  return credentials_.size();
}

const std::vector<std::string>& FleetCallerGate::unpairedCallers() const
{
  return unpaired_;
}

FleetAdmission FleetCallerGate::admitCredential(const std::string& presented,
                                                CallerSet allowed) const
{
  for (const auto& credential : credentials_) {
    if (!constantTimeEquals(presented, credential.secret))
      continue;
    const bool permitted = allowed.empty() || listed(allowed, credential.service);
    return {.verdict = permitted ? FleetVerdict::Admitted : FleetVerdict::Forbidden,
            .caller = credential.service,
            .legacy = false};
  }
  return {.verdict = FleetVerdict::Unauthenticated, .caller = {}, .legacy = false};
}

FleetAdmission FleetCallerGate::admitLegacy(const std::string& presented,
                                            CallerSet allowed) const
{
  if (!acceptsLegacy() || !constantTimeEquals(presented, legacySecret_))
    return {.verdict = FleetVerdict::Unauthenticated, .caller = {}, .legacy = true};
  const bool permitted =
      allowed.empty() || std::ranges::any_of(unpaired_, [allowed](const std::string& caller) {
        return listed(allowed, caller);
      });
  if (!permitted)
    return {.verdict = FleetVerdict::Forbidden, .caller = {}, .legacy = true};
  if (onFirstLegacy_ && !warned_.exchange(true))
    onFirstLegacy_(unpaired_);
  return {.verdict = FleetVerdict::Admitted, .caller = {}, .legacy = true};
}

FleetAdmission FleetCallerGate::admit(const PresentedCredential& presented,
                                      CallerSet allowed) const
{
  if (open())
    return {.verdict = FleetVerdict::Admitted, .caller = {}, .legacy = false};
  if (!presented.credential.empty())
    return admitCredential(presented.credential, allowed);
  if (!presented.fleetSecret.empty())
    return admitLegacy(presented.fleetSecret, allowed);
  return {.verdict = FleetVerdict::Unauthenticated, .caller = {}, .legacy = false};
}

FleetAdmission FleetCallerGate::admit(const grpc::ServerContextBase* context,
                                      CallerSet allowed) const
{
  if (open())
    return {.verdict = FleetVerdict::Admitted, .caller = {}, .legacy = false};
  const auto presented = presentedCredential(context->client_metadata());
  if (!presented)
    return {.verdict = FleetVerdict::Unauthenticated, .caller = {}, .legacy = false};
  return admit(*presented, allowed);
}

grpc::Status FleetCallerGate::refusal(FleetVerdict verdict)
{
  if (verdict == FleetVerdict::Forbidden)
    return {grpc::StatusCode::PERMISSION_DENIED, "caller may not call this method"};
  return {grpc::StatusCode::UNAUTHENTICATED, "caller credential missing or invalid"};
}

}
