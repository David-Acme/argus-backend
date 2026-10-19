#include <doctest/doctest.h>

#include <feature/voice/services/leg-arbiter/leg-arbiter.hxx>

namespace
{
constexpr VoiceSpeakerVerdict kHolder = VoiceSpeakerVerdict::Holder;
constexpr VoiceSpeakerVerdict kOther = VoiceSpeakerVerdict::OtherKnown;
constexpr VoiceSpeakerVerdict kFamiliar = VoiceSpeakerVerdict::Unfamiliar;
constexpr VoiceSpeakerVerdict kNone = VoiceSpeakerVerdict::Unknown;

TakeoverOutcome cameraTakeover(VoiceSpeakerVerdict voice, VoiceSpeakerVerdict face)
{
  return decideTakeover({.candidate = ConversationLegKind::Camera, .voice = voice, .face = face});
}
}

TEST_CASE("a phone or laptop takeover is allowed whatever the verdicts say")
{
  for (const auto leg : {ConversationLegKind::Phone, ConversationLegKind::Laptop}) {
    for (const auto verdict : {kHolder, kOther, kFamiliar, kNone}) {
      CHECK(decideTakeover({.candidate = leg, .voice = verdict, .face = verdict}) ==
            TakeoverOutcome::Allowed);
    }
  }
}

TEST_CASE("a camera takeover needs a positive holder identification")
{
  CHECK(cameraTakeover(kHolder, kNone) == TakeoverOutcome::Allowed);
  CHECK(cameraTakeover(kNone, kHolder) == TakeoverOutcome::Allowed);
  CHECK(cameraTakeover(kHolder, kHolder) == TakeoverOutcome::Allowed);
  CHECK(cameraTakeover(kHolder, kFamiliar) == TakeoverOutcome::Allowed);
  CHECK(cameraTakeover(kNone, kNone) == TakeoverOutcome::RefusedUnverified);
  CHECK(cameraTakeover(kFamiliar, kFamiliar) == TakeoverOutcome::RefusedUnverified);
}

TEST_CASE("a camera takeover is refused when the other signal contradicts the holder")
{
  CHECK(cameraTakeover(kHolder, kOther) == TakeoverOutcome::RefusedUnverified);
  CHECK(cameraTakeover(kOther, kHolder) == TakeoverOutcome::RefusedUnverified);
  CHECK(cameraTakeover(kOther, kNone) == TakeoverOutcome::RefusedUnverified);
}

TEST_CASE("a camera turn is verified by the holder with no contradiction")
{
  CHECK(identifyCameraTurn({.voice = kHolder, .face = kNone, .shortTurn = false, .inheritanceOpen = false}) == TurnIdentity::Verified);
  CHECK(identifyCameraTurn({.voice = kHolder, .face = kOther, .shortTurn = false, .inheritanceOpen = false}) == TurnIdentity::Unverified);
  CHECK(identifyCameraTurn({.voice = kFamiliar, .face = kFamiliar, .shortTurn = false, .inheritanceOpen = false}) == TurnIdentity::Unverified);
}

TEST_CASE("a short camera turn inherits the last verified speaker only inside the window")
{
  CHECK(identifyCameraTurn({.voice = kNone, .face = kNone, .shortTurn = true,
                            .inheritanceOpen = true}) == TurnIdentity::Verified);
  CHECK(identifyCameraTurn({.voice = kNone, .face = kNone, .shortTurn = true,
                            .inheritanceOpen = false}) == TurnIdentity::Unverified);
  CHECK(identifyCameraTurn({.voice = kNone, .face = kNone, .shortTurn = false,
                            .inheritanceOpen = true}) == TurnIdentity::Unverified);
}

TEST_CASE("an inherited speaker never overrides a contradiction")
{
  CHECK(identifyCameraTurn({.voice = kOther, .face = kNone, .shortTurn = true,
                            .inheritanceOpen = true}) == TurnIdentity::Unverified);
}

TEST_CASE("the inheritance window is closed at its end and never open before the verified time")
{
  constexpr int64_t kAt = 1000;
  CHECK(inheritanceOpen({.verifiedAtSeconds = kAt, .nowSeconds = kAt, .windowSeconds = 10}));
  CHECK(inheritanceOpen({.verifiedAtSeconds = kAt, .nowSeconds = kAt + 10, .windowSeconds = 10}));
  CHECK_FALSE(inheritanceOpen({.verifiedAtSeconds = kAt, .nowSeconds = kAt + 11, .windowSeconds = 10}));
  CHECK_FALSE(inheritanceOpen({.verifiedAtSeconds = kAt, .nowSeconds = kAt - 1, .windowSeconds = 10}));
}
