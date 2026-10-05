#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <grpc/fleet-caller-gate.hxx>

#include <array>
#include <string>
#include <string_view>
#include <vector>

using argus::client::FleetCallerGate;
using argus::client::FleetGateConfig;
using argus::client::FleetVerdict;
using argus::client::PresentedCredential;

namespace
{

constexpr std::array<std::string_view, 1> kVoiceOnly{"voice"};
constexpr std::array<std::string_view, 2> kAuthOrVoice{"auth", "voice"};
std::string voiceSecret()
{
  std::string value(64, 'v');
  return value;
}

std::string authSecret()
{
  std::string value(64, 'a');
  return value;
}

std::string fleetSecret()
{
  std::string value(64, 'f');
  return value;
}


FleetGateConfig paired(std::vector<std::pair<std::string, std::string>> pairs,
                       std::string legacy)
{
  return {.expectedCallers = {"auth", "voice"},
          .callerPairs = std::move(pairs),
          .legacySecret = std::move(legacy),
          .onFirstLegacy = {}};
}

PresentedCredential credential(std::string value)
{
  return {.credential = std::move(value), .fleetSecret = {}};
}

PresentedCredential fleet(std::string value)
{
  return {.credential = {}, .fleetSecret = std::move(value)};
}

}

TEST_CASE("a paired caller is named by the credential that matched")
{
  const FleetCallerGate gate(
      paired({{"auth", authSecret()}, {"voice", voiceSecret()}}, fleetSecret()));

  const auto voice = gate.admit(credential(voiceSecret()), kVoiceOnly);
  CHECK(voice.verdict == FleetVerdict::Admitted);
  CHECK(voice.caller == "voice");
  CHECK_FALSE(voice.legacy);

  const auto open = gate.admit(credential(authSecret()), {});
  CHECK(open.verdict == FleetVerdict::Admitted);
  CHECK(open.caller == "auth");
}

TEST_CASE("missing, wrong and other-caller credentials are refused")
{
  const FleetCallerGate gate(
      paired({{"auth", authSecret()}, {"voice", voiceSecret()}}, {}));

  CHECK(gate.admit(PresentedCredential{}, kVoiceOnly).verdict ==
        FleetVerdict::Unauthenticated);
  CHECK(gate.admit(credential(std::string(64, 'x')), kVoiceOnly).verdict ==
        FleetVerdict::Unauthenticated);

  const auto other = gate.admit(credential(authSecret()), kVoiceOnly);
  CHECK(other.verdict == FleetVerdict::Forbidden);
  CHECK(other.caller == "auth");
}

TEST_CASE("the fleet secret is refused once every expected caller is paired")
{
  const FleetCallerGate gate(
      paired({{"auth", authSecret()}, {"voice", voiceSecret()}}, fleetSecret()));

  CHECK_FALSE(gate.acceptsLegacy());
  CHECK(gate.admit(fleet(fleetSecret()), {}).verdict ==
        FleetVerdict::Unauthenticated);
  CHECK(gate.admit(fleet(fleetSecret()), kVoiceOnly).verdict ==
        FleetVerdict::Unauthenticated);
}

TEST_CASE("the fleet secret reaches only what an unpaired caller may call")
{
  int warnings = 0;
  std::vector<std::string> named;
  FleetGateConfig config = paired({{"voice", voiceSecret()}}, fleetSecret());
  config.onFirstLegacy = [&](const std::vector<std::string>& unpaired) {
    ++warnings;
    named = unpaired;
  };
  const FleetCallerGate gate(std::move(config));

  REQUIRE(gate.acceptsLegacy());
  CHECK(gate.unpairedCallers() == std::vector<std::string>{"auth"});

  CHECK(gate.admit(fleet(fleetSecret()), kVoiceOnly).verdict ==
        FleetVerdict::Forbidden);
  const auto admitted = gate.admit(fleet(fleetSecret()), kAuthOrVoice);
  CHECK(admitted.verdict == FleetVerdict::Admitted);
  CHECK(admitted.legacy);
  CHECK(gate.admit(fleet(fleetSecret()), {}).verdict == FleetVerdict::Admitted);
  CHECK(gate.admit(fleet(std::string(64, 'y')), {}).verdict ==
        FleetVerdict::Unauthenticated);

  CHECK(warnings == 1);
  CHECK(named == std::vector<std::string>{"auth"});
}

TEST_CASE("placeholder values never authenticate")
{
  const FleetCallerGate gate(paired({{"voice", "CHANGE_ME_VOICE_IDENTITY"}},
                                    "CHANGE_ME_SAME_IN_EVERY_SERVICE"));

  CHECK(gate.pairedCount() == 0);
  CHECK_FALSE(gate.acceptsLegacy());
  CHECK(gate.open());
  CHECK_FALSE(FleetCallerGate::pairedSecret("CHANGE_ME_VOICE_IDENTITY"));
  CHECK_FALSE(FleetCallerGate::pairedSecret(""));
  CHECK(FleetCallerGate::pairedSecret(voiceSecret()));
}

TEST_CASE("a gate with nothing configured stays open, as a loopback listener was")
{
  const FleetCallerGate gate(paired({}, {}));

  CHECK(gate.open());
  CHECK(gate.admit(PresentedCredential{}, kVoiceOnly).verdict ==
        FleetVerdict::Admitted);
}

TEST_CASE("an unpaired fleet keeps the legacy secret for every method")
{
  const FleetCallerGate gate(paired({}, fleetSecret()));

  CHECK_FALSE(gate.open());
  CHECK(gate.acceptsLegacy());
  CHECK(gate.admit(fleet(fleetSecret()), kVoiceOnly).verdict ==
        FleetVerdict::Admitted);
  CHECK(gate.admit(PresentedCredential{}, {}).verdict ==
        FleetVerdict::Unauthenticated);
}

TEST_CASE("refusals map onto the gRPC codes callers already handle")
{
  CHECK(FleetCallerGate::refusal(FleetVerdict::Unauthenticated).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(FleetCallerGate::refusal(FleetVerdict::Forbidden).error_code() ==
        grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("a credential is read from the metadata only when it is presented once")
{
  using argus::client::ClientMetadata;
  using argus::client::presentedCredential;
  const std::string voice = voiceSecret();
  const std::string fleetValue = fleetSecret();
  const std::string other(64, 'x');

  ClientMetadata single{{"x-argus-credential", voice}, {"x-argus-fleet", fleetValue}};
  const auto read = presentedCredential(single);
  REQUIRE(read.has_value());
  CHECK(read.value_or(PresentedCredential{}).credential == voice);
  CHECK(read.value_or(PresentedCredential{}).fleetSecret == fleetValue);

  const auto absent = presentedCredential(ClientMetadata{});
  REQUIRE(absent.has_value());
  CHECK(absent.value_or(credential("x")).credential.empty());
  CHECK(absent.value_or(fleet("x")).fleetSecret.empty());

  CHECK_FALSE(presentedCredential(ClientMetadata{{"x-argus-credential", voice},
                                                 {"x-argus-credential", other}})
                  .has_value());
  CHECK_FALSE(presentedCredential(ClientMetadata{{"x-argus-credential", voice},
                                                 {"x-argus-credential", voice}})
                  .has_value());
  CHECK_FALSE(presentedCredential(ClientMetadata{{"x-argus-fleet", fleetValue},
                                                 {"x-argus-fleet", fleetValue}})
                  .has_value());
}
