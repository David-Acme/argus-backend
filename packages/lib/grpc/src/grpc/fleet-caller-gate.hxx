#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace argus::client
{

inline constexpr std::string_view kUnpairedMarker = "CHANGE_ME";

using CallerSet = std::span<const std::string_view>;

struct FleetGateConfig
{
  std::vector<std::string> expectedCallers;
  std::vector<std::pair<std::string, std::string>> callerPairs;
  std::string legacySecret;
  std::function<void(const std::vector<std::string>&)> onFirstLegacy;
};

struct PresentedCredential
{
  std::string credential;
  std::string fleetSecret;
};

using ClientMetadata = std::multimap<grpc::string_ref, grpc::string_ref>;

[[nodiscard]] std::optional<PresentedCredential> presentedCredential(
    const ClientMetadata& metadata);

enum class FleetVerdict : std::uint8_t
{
  Admitted,
  Unauthenticated,
  Forbidden
};

struct FleetAdmission
{
  FleetVerdict verdict{FleetVerdict::Unauthenticated};
  std::string caller;
  bool legacy{false};

  [[nodiscard]] bool admitted() const
  {
    return verdict == FleetVerdict::Admitted;
  }
};

class FleetCallerGate
{
public:
  explicit FleetCallerGate(FleetGateConfig config);

  FleetCallerGate(const FleetCallerGate&) = delete;
  FleetCallerGate& operator=(const FleetCallerGate&) = delete;

  [[nodiscard]] FleetAdmission admit(const PresentedCredential& presented,
                                     CallerSet allowed) const;

  [[nodiscard]] FleetAdmission admit(const grpc::ServerContextBase* context,
                                     CallerSet allowed) const;

  [[nodiscard]] bool open() const;

  [[nodiscard]] bool acceptsLegacy() const;

  [[nodiscard]] std::size_t pairedCount() const;

  [[nodiscard]] const std::vector<std::string>& unpairedCallers() const;

  [[nodiscard]] static grpc::Status refusal(FleetVerdict verdict);

  [[nodiscard]] static bool pairedSecret(std::string_view secret);

private:
  [[nodiscard]] FleetAdmission admitCredential(const std::string& presented,
                                               CallerSet allowed) const;
  [[nodiscard]] FleetAdmission admitLegacy(const std::string& presented,
                                           CallerSet allowed) const;

  std::vector<CallerCredential> credentials_;
  std::vector<std::string> unpaired_;
  std::string legacySecret_;
  std::function<void(const std::vector<std::string>&)> onFirstLegacy_;
  mutable std::atomic<bool> warned_{false};
};

}
