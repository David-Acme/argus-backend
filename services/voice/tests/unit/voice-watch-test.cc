#include <doctest/doctest.h>

#include <feature/watch/camera-level.hxx>
#include <feature/watch/echo-guard.hxx>
#include <feature/watch/fire-log.hxx>
#include <feature/watch/stage-two.hxx>
#include <feature/watch/wake-words.hxx>
#include <feature/watch/watch-coordinator.hxx>

#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace
{

EarFire fireAt(int64_t cameraId, float snrDb, std::chrono::milliseconds offset)
{
  static const auto base = WatchClock::now();
  return {.cameraId = cameraId,
          .at = base + offset,
          .source = "kws",
          .snrDb = snrDb,
          .floorDb = -60.0F,
          .duringPlayback = false,
          .capture = nullptr,
          .envelope = {},
          .envelopeStart = base,
          .transcript = std::nullopt};
}

std::vector<float> tone(size_t samples, float amplitude)
{
  std::vector<float> out(samples);
  for (size_t index = 0; index < samples; ++index)
    out[index] = amplitude * static_cast<float>(std::sin(static_cast<double>(index) * 0.2));
  return out;
}

}

TEST_CASE("the name opening the utterance is talking to Argus, the name inside a clause is not")
{
  const WakeMatch opening = matchWake({.text = "Argus, ¿qué tengo hoy?", .maxDistance = 2});
  CHECK(opening.position == WakePosition::Vocative);
  CHECK(opening.distance == 0);
  CHECK(opening.request == "que tengo hoy");

  CHECK(matchWake({.text = "oye Argos, apaga la luz", .maxDistance = 2}).position == WakePosition::Vocative);
  CHECK(matchWake({.text = "Hola Árgus", .maxDistance = 2}).position == WakePosition::Vocative);
  CHECK(matchWake({.text = "¿qué hora es, Argus?", .maxDistance = 2}).position == WakePosition::Vocative);
  CHECK(matchWake({.text = "mira, Argus, prende la luz", .maxDistance = 2}).position == WakePosition::Vocative);

  CHECK(matchWake({.text = "mira, yo he implementado Argus", .maxDistance = 2}).position ==
        WakePosition::Referential);
  CHECK(matchWake({.text = "el Argus de mi casa es local", .maxDistance = 2}).position ==
        WakePosition::Referential);
  CHECK(matchWake({.text = "eso es un buen argumento", .maxDistance = 2}).position == WakePosition::None);
  CHECK(matchWake({.text = "argu", .maxDistance = 2}).position == WakePosition::None);
  CHECK(matchWake({.text = "trae agua", .maxDistance = 2}).position == WakePosition::None);
  CHECK(matchWake({.text = "algo", .maxDistance = 2}).position == WakePosition::None);
}

TEST_CASE("the wake distance is measured against both spellings")
{
  CHECK(editDistance("argus", "argos") == 1);
  CHECK(editDistance("", "abc") == 3);
  CHECK(wakeDistance("argus") == 0);
  CHECK(wakeDistance("argos") == 0);
  CHECK(wakeDistance("angus") == 1);
  CHECK(isWakeWord("arguz", 1));
  CHECK_FALSE(isWakeWord("argumentos", 2));
  CHECK(wakeConfidence(matchWake({.text = "Argus hola", .maxDistance = 2})) == doctest::Approx(1.0F));
  CHECK(wakeConfidence(matchWake({.text = "yo uso Argus", .maxDistance = 2})) == doctest::Approx(0.0F));
  CHECK(foldText("¿Qué TAL, Ñandú?") == "que tal nandu");
}

TEST_CASE("a reply never says Argus aloud")
{
  CHECK(withoutSelfName("Soy Argus, el sistema de esta casa.", VoiceLang::Es) ==
        "Soy tu asistente, el sistema de esta casa.");
  CHECK(withoutSelfName("Argus está listo.", VoiceLang::Es) == "Tu asistente está listo.");
  CHECK(withoutSelfName("Aquí ARGOS y Árgus.", VoiceLang::Es) == "Aquí tu asistente y tu asistente.");
  CHECK(withoutSelfName("This is Argus.", VoiceLang::En) == "This is your assistant.");
  CHECK(withoutSelfName("Un buen argumento, sin el nombre.", VoiceLang::Es) == "Un buen argumento, sin el nombre.");
  CHECK(withoutSelfName("", VoiceLang::Es).empty());
}

TEST_CASE("closing, hand-off, trailing and panic phrases are read off the words")
{
  CHECK(closingPhrase("Gracias, Argus."));
  CHECK(closingPhrase("eso es todo"));
  CHECK_FALSE(closingPhrase("gracias por todo"));
  CHECK(handoffRequest("Pásalo al celular, por favor"));
  CHECK(handoffRequest("continue on my phone"));
  CHECK_FALSE(handoffRequest("mi celular está cargando"));
  CHECK(trailsOff("quiero ir a la tienda y"));
  CHECK(trailsOff("recuérdame comprar pan para"));
  CHECK_FALSE(trailsOff("recuérdame comprar pan"));
  CHECK(panicPhrase("¡Auxilio!"));
  CHECK(panicPhrase("llama a la policía"));
  CHECK_FALSE(panicPhrase("la política del país"));
}

TEST_CASE("stage two accepts only a vocative wake")
{
  CHECK(judgeWake("Argus, ¿qué tengo hoy?", 2).accepted);
  const StageTwoResult referential = judgeWake("yo he implementado Argus", 2);
  CHECK_FALSE(referential.accepted);
  CHECK(referential.match.position == WakePosition::Referential);
  CHECK_FALSE(judgeWake("", 2).accepted);
}

TEST_CASE("the echo of what Argus played correlates with it after a delay")
{
  std::vector<float> played;
  for (int index = 0; index < 120; ++index)
    played.push_back(static_cast<float>((index * 37) % 11) / 10.0F);
  std::vector<float> mic(100, 0.05F);
  for (size_t index = 0; index < mic.size(); ++index)
    mic[index] = played[index + 10] * 0.4F + 0.01F;
  const float echo = alignedCorrelation(
      {.mic = mic, .micStartMs = 400, .played = played, .playedStartMs = 0, .frameMs = 20, .maxLagMs = 1000, .minOverlap = 25});
  CHECK(echo > 0.9F);

  std::vector<float> unrelated;
  for (int index = 0; index < 100; ++index)
    unrelated.push_back(static_cast<float>((index * 13) % 7) / 6.0F);
  const float other = alignedCorrelation({.mic = unrelated,
                                          .micStartMs = 400,
                                          .played = played,
                                          .playedStartMs = 0,
                                          .frameMs = 20,
                                          .maxLagMs = 0,
                                          .minOverlap = 25});
  CHECK(other < 0.6F);
  CHECK(envelopeOf(tone(960, 0.5F), 320).size() == 3);
}

TEST_CASE("a camera is muted while it plays and for the tail after it")
{
  EchoGuard guard({.tail = std::chrono::milliseconds(800),
                   .maxLag = std::chrono::milliseconds(1000),
                   .history = std::chrono::milliseconds(10000),
                   .sampleRate = 16000});
  const auto now = WatchClock::now();
  CHECK_FALSE(guard.muted(1, now));
  guard.beginPlayback(1, now);
  guard.addPlayed(1, tone(16000, 0.3F), now);
  CHECK(guard.muted(1, now));
  guard.endPlayback(1, now);
  CHECK(guard.muted(1, now + std::chrono::milliseconds(500)));
  CHECK_FALSE(guard.muted(1, now + std::chrono::milliseconds(900)));
  const std::vector<float> envelope(60, 0.1F);
  const EchoQuery fromTwo{.cameraId = 2,
                          .micEnvelope = envelope,
                          .micStart = now,
                          .sameEnvironment = [](int64_t, int64_t) { return true; }};
  CHECK(guard.othersPlaying(fromTwo, now + std::chrono::milliseconds(100)));
  const EchoQuery elsewhere{.cameraId = 2,
                            .micEnvelope = envelope,
                            .micStart = now,
                            .sameEnvironment = [](int64_t, int64_t) { return false; }};
  CHECK_FALSE(guard.othersPlaying(elsewhere, now + std::chrono::milliseconds(100)));
}

TEST_CASE("the noise floor follows silence and speech is measured over it")
{
  CameraLevel level({.floorRiseDbPerSecond = 0.5F,
                     .floorFallDbPerSecond = 100.0F,
                     .initialFloorDb = -20.0F,
                     .window = std::chrono::milliseconds(2000),
                     .sampleRate = 16000});
  const std::vector<float> quiet = tone(512, 0.001F);
  for (int index = 0; index < 50; ++index)
    level.observe(quiet, false);
  CHECK(level.floorDb() < -55.0F);
  const std::vector<float> loud = tone(512, 0.3F);
  for (int index = 0; index < 20; ++index)
    level.observe(loud, true);
  CHECK(level.speechSnrDb() > 30.0F);
  CHECK(rmsDbOf({}) == doctest::Approx(-120.0F));
}

TEST_CASE("one wake at a similar level on three cameras at once is broadcast audio")
{
  const VoiceWatchConfig config;
  const std::map<int64_t, WatchCameraInfo> cameras{
      {1, {.id = 1, .name = "Sala", .talk = true, .panTilt = false, .tvExclusion = false}},
      {2, {.id = 2, .name = "Cocina", .talk = true, .panTilt = false, .tvExclusion = false}},
      {3, {.id = 3, .name = "TV", .talk = true, .panTilt = false, .tvExclusion = true}}};
  const std::vector<EarFire> tv{fireAt(1, 12.0F, std::chrono::milliseconds(0)),
                                fireAt(2, 13.0F, std::chrono::milliseconds(40)),
                                fireAt(3, 14.0F, std::chrono::milliseconds(90))};
  CHECK(decideWakes({.fires = tv, .cameras = cameras, .config = config}).broadcast);

  const std::vector<EarFire> person{fireAt(1, 6.0F, std::chrono::milliseconds(0)),
                                    fireAt(2, 18.0F, std::chrono::milliseconds(30))};
  const WakeDecision decision = decideWakes({.fires = person, .cameras = cameras, .config = config});
  CHECK_FALSE(decision.broadcast);
  REQUIRE(decision.candidates.size() == 1);
  CHECK(person[decision.candidates.front().fire].cameraId == 2);
  REQUIRE(decision.duplicates.size() == 1);
  CHECK(person[decision.duplicates.front()].cameraId == 1);

  const std::vector<EarFire> nearTv{fireAt(3, 7.0F, std::chrono::milliseconds(0))};
  const WakeDecision raised = decideWakes({.fires = nearTv, .cameras = cameras, .config = config});
  CHECK(raised.candidates.empty());
  CHECK(raised.quiet.size() == 1);
}

TEST_CASE("the fire log keeps scores and verdicts, never audio")
{
  FireLog log(2);
  for (int64_t camera = 1; camera <= 3; ++camera)
    log.record({.cameraId = camera,
                .at = std::chrono::system_clock::now(),
                .verdict = WakeVerdict::Rejected,
                .source = "kws",
                .snrDb = 9.0F,
                .distance = 1,
                .stageTwo = 0.0F,
                .score = 9.0F});
  const auto recent = log.recent();
  REQUIRE(recent.size() == 2);
  CHECK(recent.front().cameraId == 2);
  CHECK(wakeVerdictToString(WakeVerdict::DuplicateWake) == "duplicate_wake");
}
