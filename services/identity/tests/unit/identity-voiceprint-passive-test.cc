#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <feature/voiceprint/services/passive/passive-policy.hxx>
#include <feature/voiceprint/services/passive/voice-call-tracker.hxx>
#include <random>
#include <string>
#include <vector>

namespace
{

constexpr size_t kDims = 192;
constexpr int64_t kDay = 86400;
constexpr int64_t kHour = 3600;
constexpr int64_t kMonday = 1790000000;

std::vector<float> randomUnit(std::mt19937& rng)
{
  std::normal_distribution<float> gauss(0.0F, 1.0F);
  std::vector<float> values(kDims);
  for (float& value : values)
    value = gauss(rng);
  return voice_vector::normalized(values);
}

class Voice
{
public:
  explicit Voice(uint32_t seed) : rng_(seed)
  {
    base_ = randomUnit(rng_);
  }

  [[nodiscard]] std::vector<float> utterance()
  {
    const std::vector<float> noise = randomUnit(rng_);
    std::vector<float> out(kDims);
    for (size_t index = 0; index < kDims; ++index)
      out[index] = base_[index] + kSpread * noise[index];
    return voice_vector::normalized(out);
  }

  [[nodiscard]] std::vector<float> call(int turns)
  {
    std::vector<std::vector<float>> parts;
    parts.reserve(static_cast<size_t>(turns));
    for (int turn = 0; turn < turns; ++turn)
      parts.push_back(utterance());
    return voice_vector::centroid(parts);
  }

  [[nodiscard]] const std::vector<float>& base() const { return base_; }

private:
  std::mt19937 rng_;
  static constexpr float kSpread = 0.5F;
  std::vector<float> base_;
};

PassiveVoiceConfig defaults()
{
  return PassiveVoiceConfig{};
}

struct SampleSpec
{
  int64_t id{0};
  std::vector<float> embedding;
  int64_t createdAt{0};
  bool ownDevice{true};
};

PolicySample sample(SampleSpec spec)
{
  return {.id = spec.id,
          .embedding = std::move(spec.embedding),
          .createdAt = spec.createdAt,
          .speechSeconds = 10.0F,
          .ownDevice = spec.ownDevice};
}

TrackedTurn turnOf(const std::string& key, std::vector<float> embedding,
                   int64_t now)
{
  return {.callKey = key,
          .userId = 7,
          .deviceHash = "phone",
          .embedding = std::move(embedding),
          .speechSeconds = 3.0F,
          .halvesScore = 0.7F,
          .bestOther = std::nullopt,
          .ownScore = std::nullopt,
          .now = now};
}

}

TEST_CASE("a turn joins the call only while the call stays one speaker")
{
  const PassivePolicy policy(defaults());
  Voice rita(7);
  Voice laura(8);
  const std::vector<std::vector<float>> accepted{rita.utterance(),
                                                 rita.utterance()};
  const auto turn = rita.utterance();

  CHECK(policy.judgeTurn({.accepted = accepted,
                          .embedding = turn,
                          .halvesScore = 0.72F,
                          .bestOther = std::nullopt,
                          .ownScore = std::nullopt}) == TurnVerdict::Accepted);

  SUBCASE("two voices inside one turn")
  {
    CHECK(policy.judgeTurn({.accepted = accepted,
                            .embedding = turn,
                            .halvesScore = 0.12F,
                            .bestOther = std::nullopt,
                            .ownScore = std::nullopt}) ==
          TurnVerdict::MixedTurn);
  }
  SUBCASE("a turn that sounds more like another enrolled person")
  {
    CHECK(policy.judgeTurn({.accepted = accepted,
                            .embedding = turn,
                            .halvesScore = 0.7F,
                            .bestOther = OtherVoiceMatch{.userId = 8,
                                                         .score = 0.71F},
                            .ownScore = 0.40F}) ==
          TurnVerdict::OtherSpeaker);
    CHECK(policy.judgeTurn({.accepted = accepted,
                            .embedding = turn,
                            .halvesScore = 0.7F,
                            .bestOther = OtherVoiceMatch{.userId = 8,
                                                         .score = 0.71F},
                            .ownScore = std::nullopt}) ==
          TurnVerdict::OtherSpeaker);
  }
  SUBCASE("a faint resemblance to someone else is not another speaker")
  {
    CHECK(policy.judgeTurn({.accepted = accepted,
                            .embedding = turn,
                            .halvesScore = 0.7F,
                            .bestOther = OtherVoiceMatch{.userId = 8,
                                                         .score = 0.30F},
                            .ownScore = std::nullopt}) ==
          TurnVerdict::Accepted);
    CHECK(policy.judgeTurn({.accepted = accepted,
                            .embedding = turn,
                            .halvesScore = 0.7F,
                            .bestOther = OtherVoiceMatch{.userId = 8,
                                                         .score = 0.55F},
                            .ownScore = 0.80F}) == TurnVerdict::Accepted);
  }
  SUBCASE("an unknown voice after the holder's turns drifts the call")
  {
    CHECK(policy.judgeTurn({.accepted = accepted,
                            .embedding = laura.utterance(),
                            .halvesScore = 0.7F,
                            .bestOther = std::nullopt,
                            .ownScore = std::nullopt}) == TurnVerdict::Drift);
  }
  SUBCASE("a call keeps a bounded number of turns")
  {
    std::vector<std::vector<float>> full;
    for (int index = 0; index < defaults().maxCallTurns; ++index)
      full.push_back(rita.utterance());
    CHECK(policy.judgeTurn({.accepted = full,
                            .embedding = turn,
                            .halvesScore = 0.7F,
                            .bestOther = std::nullopt,
                            .ownScore = std::nullopt}) ==
          TurnVerdict::CallFull);
  }
}

TEST_CASE("a closed call teaches only when it is long, clean and one voice")
{
  const PassivePolicy policy(defaults());
  Voice rita(7);
  Voice laura(8);
  const std::vector<std::vector<float>> clean{rita.utterance(),
                                              rita.utterance(),
                                              rita.utterance()};

  const auto usable = policy.judgeCall(
      {.turns = clean, .speechSeconds = 9.0F, .tainted = false});
  CHECK(usable.verdict == CallVerdict::Usable);
  CHECK(voice_vector::cosine(usable.centroid, rita.base()) > 0.8F);

  CHECK(policy
            .judgeCall({.turns = clean, .speechSeconds = 9.0F, .tainted = true})
            .verdict == CallVerdict::Tainted);
  const std::vector<std::vector<float>> one{rita.utterance()};
  CHECK(policy.judgeCall({.turns = one, .speechSeconds = 9.0F, .tainted = false})
            .verdict == CallVerdict::TooFewTurns);
  CHECK(policy
            .judgeCall({.turns = clean, .speechSeconds = 4.0F, .tainted = false})
            .verdict == CallVerdict::TooLittleSpeech);
  const std::vector<std::vector<float>> mixed{
      rita.utterance(), rita.utterance(), laura.utterance()};
  CHECK(policy.judgeCall({.turns = mixed, .speechSeconds = 9.0F, .tainted = false})
            .verdict == CallVerdict::Inconsistent);
}

TEST_CASE("a voice is linked only after consistent calls on separate occasions")
{
  const PassivePolicy policy(defaults());
  Voice rita(7);
  Voice laura(8);
  Voice tv(9);

  SUBCASE("nothing heard, nothing linked")
  {
    CHECK(policy.evaluateLink({.samples = {}, .otherProfiles = {},
                               .utcOffsetSeconds = 0})
              .verdict == LinkVerdict::NoSamples);
  }
  SUBCASE("three calls on two days on the holder's own phone link the holder")
  {
    const std::vector<PolicySample> samples{
        sample({.id = 1, .embedding = rita.call(3), .createdAt = kMonday,
                .ownDevice = true}),
        sample({.id = 2, .embedding = rita.call(3),
                .createdAt = kMonday + 2 * kHour, .ownDevice = true}),
        sample({.id = 3, .embedding = rita.call(3),
                .createdAt = kMonday + kDay, .ownDevice = true})};
    const auto decision = policy.evaluateLink(
        {.samples = samples, .otherProfiles = {}, .utcOffsetSeconds = 0});
    CHECK(decision.verdict == LinkVerdict::Link);
    CHECK(decision.members.size() == 3);
    CHECK(decision.occasions == 3);
    CHECK(decision.days == 2);
    CHECK(voice_vector::cosine(decision.centroid, rita.base()) > 0.85F);
  }
  SUBCASE("back-to-back calls are one occasion")
  {
    const std::vector<PolicySample> samples{
        sample({.id = 1, .embedding = rita.call(3), .createdAt = kMonday,
                .ownDevice = true}),
        sample({.id = 2, .embedding = rita.call(3),
                .createdAt = kMonday + 600, .ownDevice = true}),
        sample({.id = 3, .embedding = rita.call(3),
                .createdAt = kMonday + 1200, .ownDevice = true}),
        sample({.id = 4, .embedding = rita.call(3),
                .createdAt = kMonday + kDay, .ownDevice = true})};
    const auto decision = policy.evaluateLink(
        {.samples = samples, .otherProfiles = {}, .utcOffsetSeconds = 0});
    CHECK(decision.verdict == LinkVerdict::TooFewOccasions);
    CHECK(decision.occasions == 2);
  }
  SUBCASE("one evening is not enough, however many calls")
  {
    std::vector<PolicySample> samples;
    samples.reserve(5);
    for (int64_t index = 0; index < 5; ++index)
      samples.push_back(sample({.id = index + 1,
                                .embedding = rita.call(3),
                                .createdAt = kMonday + index * 2 * kHour,
                                .ownDevice = true}));
    const auto decision = policy.evaluateLink(
        {.samples = samples, .otherProfiles = {}, .utcOffsetSeconds = 0});
    CHECK(decision.verdict == LinkVerdict::TooFewDays);
    CHECK(decision.occasions == 5);
  }
  SUBCASE("days follow the server's local calendar")
  {
    const int64_t lateEvening = kMonday - (kMonday % kDay) + 22 * kHour;
    const std::vector<PolicySample> samples{
        sample({.id = 1, .embedding = rita.call(3), .createdAt = lateEvening,
                .ownDevice = true}),
        sample({.id = 2, .embedding = rita.call(3),
                .createdAt = lateEvening + int64_t{90} * 60, .ownDevice = true}),
        sample({.id = 3, .embedding = rita.call(3),
                .createdAt = lateEvening + 3 * kHour, .ownDevice = true})};
    CHECK(policy
              .evaluateLink({.samples = samples, .otherProfiles = {},
                             .utcOffsetSeconds = 0})
              .verdict == LinkVerdict::Link);
    CHECK(policy
              .evaluateLink({.samples = samples, .otherProfiles = {},
                             .utcOffsetSeconds = -4 * kHour})
              .verdict == LinkVerdict::TooFewDays);
  }
  SUBCASE("a phone two accounts share never links anybody")
  {
    std::vector<PolicySample> samples;
    samples.reserve(6);
    for (int64_t index = 0; index < 6; ++index)
      samples.push_back(sample({.id = index + 1,
                                .embedding = rita.call(3),
                                .createdAt = kMonday + index * kDay,
                                .ownDevice = false}));
    const auto decision = policy.evaluateLink(
        {.samples = samples, .otherProfiles = {}, .utcOffsetSeconds = 0});
    CHECK(decision.verdict == LinkVerdict::TooFewOccasions);
    CHECK(decision.occasions == 0);
  }
  SUBCASE("two people taking turns on one account link neither")
  {
    std::vector<PolicySample> samples;
    samples.reserve(8);
    for (int64_t index = 0; index < 8; ++index)
      samples.push_back(sample({.id = index + 1,
                                .embedding = index % 2 == 0 ? rita.call(3)
                                                            : laura.call(3),
                                .createdAt = kMonday + index * kDay,
                                .ownDevice = true}));
    const auto decision = policy.evaluateLink(
        {.samples = samples, .otherProfiles = {}, .utcOffsetSeconds = 0});
    CHECK(decision.verdict == LinkVerdict::NotDominant);
    CHECK(decision.dominance == doctest::Approx(0.5F));
  }
  SUBCASE("the voice that holds three calls in four is the holder's")
  {
    std::vector<PolicySample> samples;
    samples.reserve(8);
    for (int64_t index = 0; index < 8; ++index)
      samples.push_back(sample({.id = index + 1,
                                .embedding = index % 4 == 3 ? tv.call(3)
                                                            : rita.call(3),
                                .createdAt = kMonday + index * kDay,
                                .ownDevice = true}));
    const auto decision = policy.evaluateLink(
        {.samples = samples, .otherProfiles = {}, .utcOffsetSeconds = 0});
    CHECK(decision.verdict == LinkVerdict::Link);
    CHECK(decision.members.size() == 6);
    CHECK(voice_vector::cosine(decision.centroid, rita.base()) > 0.85F);
  }
  SUBCASE("calls too short in total do not link")
  {
    std::vector<PolicySample> samples;
    samples.reserve(3);
    for (int64_t index = 0; index < 3; ++index) {
      auto entry = sample({.id = index + 1,
                           .embedding = rita.call(3),
                           .createdAt = kMonday + index * kDay,
                           .ownDevice = true});
      entry.speechSeconds = 5.0F;
      samples.push_back(std::move(entry));
    }
    CHECK(policy
              .evaluateLink({.samples = samples, .otherProfiles = {},
                             .utcOffsetSeconds = 0})
              .verdict == LinkVerdict::TooLittleSpeech);
  }
  SUBCASE("a voice already linked to someone else is never linked again")
  {
    std::vector<PolicySample> samples;
    samples.reserve(3);
    for (int64_t index = 0; index < 3; ++index)
      samples.push_back(sample({.id = index + 1,
                                .embedding = laura.call(3),
                                .createdAt = kMonday + index * kDay,
                                .ownDevice = true}));
    const std::vector<std::vector<float>> others{laura.call(4)};
    CHECK(policy
              .evaluateLink({.samples = samples, .otherProfiles = others,
                             .utcOffsetSeconds = 0})
              .verdict == LinkVerdict::VoiceTaken);
  }
}

TEST_CASE("a call refreshes a linked voice only when it clearly is that voice")
{
  const PassivePolicy policy(defaults());
  const AdoptInput strong{.ownScore = 0.82F,
                          .bestOther = std::nullopt,
                          .sharedDevice = false,
                          .deviceMatched = 5,
                          .deviceConflicting = 0};
  CHECK(policy.judgeAdoption(strong) == AdoptVerdict::Adopt);

  auto weak = strong;
  weak.ownScore = 0.58F;
  CHECK(policy.judgeAdoption(weak) == AdoptVerdict::Weak);

  auto shared = strong;
  shared.ownScore = 0.62F;
  shared.sharedDevice = true;
  CHECK(policy.judgeAdoption(shared) == AdoptVerdict::Weak);
  shared.sharedDevice = false;
  CHECK(policy.judgeAdoption(shared) == AdoptVerdict::Adopt);

  auto other = strong;
  other.bestOther = OtherVoiceMatch{.userId = 8, .score = 0.85F};
  CHECK(policy.judgeAdoption(other) == AdoptVerdict::OtherBetter);

  auto conflicted = strong;
  conflicted.deviceMatched = 1;
  conflicted.deviceConflicting = 3;
  CHECK(policy.judgeAdoption(conflicted) == AdoptVerdict::DeviceConflicted);
  conflicted.deviceMatched = 0;
  conflicted.deviceConflicting = 2;
  CHECK(policy.judgeAdoption(conflicted) == AdoptVerdict::Adopt);
}

TEST_CASE("a refresh moves the profile gradually and leaves outliers out")
{
  const PassivePolicy policy(defaults());
  Voice rita(7);
  Voice laura(8);
  const auto current = rita.call(6);

  std::vector<std::vector<float>> reservoir;
  reservoir.reserve(10);
  for (int index = 0; index < 9; ++index)
    reservoir.push_back(rita.call(3));
  reservoir.push_back(laura.call(3));

  const auto refreshed =
      policy.refresh({.current = current, .reservoir = reservoir});
  REQUIRE(refreshed.applied);
  CHECK(refreshed.kept == 9);
  CHECK(refreshed.dropped == 1);
  CHECK(voice_vector::cosine(refreshed.centroid, current) > 0.95F);
  CHECK(voice_vector::cosine(refreshed.centroid, rita.base()) >=
        voice_vector::cosine(current, rita.base()) - 0.01F);
  CHECK(voice_vector::cosine(refreshed.centroid, laura.base()) < 0.2F);

  std::vector<std::vector<float>> stranger;
  stranger.reserve(5);
  for (int index = 0; index < 5; ++index)
    stranger.push_back(laura.call(3));
  CHECK_FALSE(policy.refresh({.current = current, .reservoir = stranger}).applied);
  CHECK_FALSE(policy.refresh({.current = current, .reservoir = {}}).applied);
}

TEST_CASE("the call tracker keeps each call to one caller and one voice")
{
  auto config = defaults();
  config.maxOpenCalls = 2;
  VoiceCallTracker tracker(config);
  Voice rita(7);
  Voice laura(8);

  CHECK(tracker.observe(turnOf("a", rita.utterance(), 100)) ==
        TurnVerdict::Accepted);
  CHECK(tracker.observe(turnOf("a", rita.utterance(), 110)) ==
        TurnVerdict::Accepted);
  CHECK(tracker.observe(turnOf("a", laura.utterance(), 120)) ==
        TurnVerdict::Drift);
  CHECK(tracker.observe(turnOf("a", rita.utterance(), 130)) ==
        TurnVerdict::CallTainted);
  const auto tainted = tracker.close("a");
  if (!tainted) {
    FAIL("expected a value in tainted");
    return;
  }
  CHECK(tainted->tainted);
  CHECK(tainted->taint == TurnVerdict::Drift);
  CHECK(tainted->turns.empty());
  CHECK(tainted->rejectedTurns == 2);
  CHECK(tracker.observe(turnOf("a", rita.utterance(), 140)) ==
        TurnVerdict::CallClosed);

  CHECK(tracker.observe(turnOf("b", rita.utterance(), 200)) ==
        TurnVerdict::Accepted);
  auto stranger = turnOf("b", rita.utterance(), 210);
  stranger.deviceHash = "tablet";
  CHECK(tracker.observe(std::move(stranger)) == TurnVerdict::CallMismatch);

  CHECK(tracker.observe(turnOf("c", rita.utterance(), 300)) ==
        TurnVerdict::Accepted);
  CHECK(tracker.observe(turnOf("d", rita.utterance(), 300)) ==
        TurnVerdict::Busy);
  CHECK(tracker.open() == 2);

  const auto idle = tracker.expire(210 + config.callIdleSeconds);
  CHECK(idle.size() == 1);
  CHECK(tracker.open() == 1);
  const auto closed = tracker.close("c");
  if (!closed) {
    FAIL("expected a value in closed");
    return;
  }
  CHECK_FALSE(closed->tainted);
  CHECK(closed->turns.size() == 1);
  CHECK(closed->speechSeconds == doctest::Approx(3.0F));
  CHECK_FALSE(tracker.close("missing").has_value());
}
