#include <doctest/doctest.h>

#include <feature/watch/camera-level.hxx>
#include <feature/watch/camera-identity.hxx>
#include <feature/watch/conversation-policy.hxx>
#include <feature/watch/speaker-resolution.hxx>
#include <feature/watch/watch-conversations.hxx>
#include <feature/watch/watch-directory.hxx>
#include <feature/watch/watch-sync.hxx>
#include <feature/watch/echo-guard.hxx>
#include <feature/watch/fire-log.hxx>
#include <feature/watch/stage-two.hxx>
#include <feature/watch/visitor-consent.hxx>
#include <feature/watch/visitor-replies.hxx>
#include <feature/watch/wake-words.hxx>
#include <feature/watch/watch-coordinator.hxx>

#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <test-support/value-of.hxx>

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

struct Tone
{
  size_t samples{0};
  float amplitude{0.0F};
};

std::vector<float> tone(const Tone& shape)
{
  std::vector<float> out(shape.samples);
  for (size_t index = 0; index < shape.samples; ++index)
    out[index] = shape.amplitude * static_cast<float>(std::sin(static_cast<double>(index) * 0.2));
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
  played.reserve(120);
  for (int index = 0; index < 120; ++index)
    played.push_back(static_cast<float>((index * 37) % 11) / 10.0F);
  std::vector<float> mic(100, 0.05F);
  for (size_t index = 0; index < mic.size(); ++index)
    mic[index] = played[index + 10] * 0.4F + 0.01F;
  const float echo = alignedCorrelation(
      {.mic = mic, .micStartMs = 400, .played = played, .playedStartMs = 0, .frameMs = 20, .maxLagMs = 1000, .minOverlap = 25});
  CHECK(echo > 0.9F);

  std::vector<float> unrelated;
  unrelated.reserve(100);
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
  CHECK(envelopeOf(tone({.samples = 960, .amplitude = 0.5F}), 320).size() == 3);
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
  guard.addPlayed(1, tone({.samples = 16000, .amplitude = 0.3F}), now);
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
  const std::vector<float> quiet = tone({.samples = 512, .amplitude = 0.001F});
  for (int index = 0; index < 50; ++index)
    level.observe(quiet, false);
  CHECK(level.floorDb() < -55.0F);
  const std::vector<float> loud = tone({.samples = 512, .amplitude = 0.3F});
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
  CHECK(decideWakes({.fires = tv, .cameras = cameras, .config = config, .normalize = {}}).broadcast);

  const std::vector<EarFire> person{fireAt(1, 6.0F, std::chrono::milliseconds(0)),
                                    fireAt(2, 18.0F, std::chrono::milliseconds(30))};
  const WakeDecision decision = decideWakes({.fires = person, .cameras = cameras, .config = config, .normalize = {}});
  CHECK_FALSE(decision.broadcast);
  REQUIRE(decision.candidates.size() == 2);
  CHECK(person[decision.candidates.front().fire].cameraId == 2);
  CHECK(decision.duplicates.empty());

  const std::vector<EarFire> nearTv{fireAt(3, 7.0F, std::chrono::milliseconds(0))};
  const WakeDecision raised = decideWakes({.fires = nearTv, .cameras = cameras, .config = config, .normalize = {}});
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

TEST_CASE("arbitration ranks by signal over each camera's typical level, not by who fired first")
{
  VoiceWatchConfig config;
  config.stageTwoCandidates = 1;
  const std::map<int64_t, WatchCameraInfo> cameras{
      {1, {.id = 1, .name = "Sala", .talk = true, .panTilt = false, .tvExclusion = false}},
      {2, {.id = 2, .name = "Cocina", .talk = true, .panTilt = false, .tvExclusion = false}}};
  const std::vector<EarFire> fires{fireAt(1, 14.0F, std::chrono::milliseconds(0)),
                                   fireAt(2, 16.0F, std::chrono::milliseconds(200))};
  const WakeDecision plain = decideWakes({.fires = fires, .cameras = cameras, .config = config, .normalize = {}});
  REQUIRE(plain.candidates.size() == 1);
  CHECK(fires[plain.candidates.front().fire].cameraId == 2);

  CameraGains gains(1.0F);
  gains.observe(1, 4.0F);
  gains.observe(2, 20.0F);
  CHECK(gains.typical(2) == doctest::Approx(20.0F));
  const WakeDecision normalized =
      decideWakes({.fires = fires,
                   .cameras = cameras,
                   .config = config,
                   .normalize = [&gains](int64_t camera, float snr) { return gains.normalize(camera, snr, 0.5F); }});
  REQUIRE(normalized.candidates.size() == 1);
  CHECK(fires[normalized.candidates.front().fire].cameraId == 1);
  CHECK(normalized.duplicates.size() == 1);
  CHECK(normalizedSnr({.snrDb = 10.0F, .typicalDb = 20.0F, .meanTypicalDb = 12.0F, .weight = 0.5F}) ==
        doctest::Approx(6.0F));
  CHECK(wakeScore({.wake = 1.0F, .normalizedSnr = 10.0F, .stageTwo = 0.75F}, config) == doctest::Approx(2.75F));
}

TEST_CASE("rooms come from the guard's camera context, and unknown cameras share the environment")
{
  RoomDirectory rooms;
  CHECK(rooms.sameEnvironment(1, 2));
  CHECK_FALSE(rooms.sameRoom(1, 2));
  rooms.replace({{.cameraId = 1, .environmentId = 1, .room = "1:living", .outdoor = false},
                 {.cameraId = 2, .environmentId = 1, .room = "1:living", .outdoor = false},
                 {.cameraId = 3, .environmentId = 2, .room = "2:office", .outdoor = false}});
  CHECK(rooms.sameRoom(1, 2));
  CHECK_FALSE(rooms.sameRoom(1, 3));
  CHECK_FALSE(rooms.sameEnvironment(2, 3));
  CHECK(rooms.roommates(1) == std::vector<int64_t>{2});
  CHECK(rooms.roomOf(9) == "camera:9");
}

TEST_CASE("the conversation speaker's level is kept per camera")
{
  SpeakerLevels levels(0.5F);
  levels.observe({.cameraId = 1, .snrDb = 10.0F});
  levels.observe({.cameraId = 2, .snrDb = 20.0F});
  levels.observe({.cameraId = 2, .snrDb = 10.0F});
  CHECK(levels.snapshot().at(2) == doctest::Approx(15.0F));
  CHECK(levels.loudest() == 2);
  levels.clear();
  CHECK(levels.loudest() == 0);
}

TEST_CASE("a conversation closes on silence or length, and a slow reply is acknowledged then given up")
{
  CHECK(conversationDue({.now = 25000, .lastSpeech = 4000, .startedAt = 0, .idleCloseMs = 20000, .maxLengthMs = 600000, .busy = false}) ==
        ConversationEnd::Silence);
  CHECK(conversationDue({.now = 25000, .lastSpeech = 4000, .startedAt = 0, .idleCloseMs = 20000, .maxLengthMs = 600000, .busy = true}) ==
        ConversationEnd::None);
  CHECK(conversationDue({.now = 600000, .lastSpeech = 599000, .startedAt = 0, .idleCloseMs = 20000, .maxLengthMs = 600000, .busy = true}) ==
        ConversationEnd::Limit);

  const ReplyTimerInput waiting{.now = 1000, .userTurnAt = 0, .firstAudioAt = 0, .acked = false, .ackAfterMs = 1500, .failAfterMs = 8000};
  CHECK(replyNudge(waiting) == ReplyNudge::None);
  CHECK(replyNudge({.now = 2600, .userTurnAt = 1000, .firstAudioAt = 0, .acked = false, .ackAfterMs = 1500, .failAfterMs = 8000}) ==
        ReplyNudge::Acknowledge);
  CHECK(replyNudge({.now = 2600, .userTurnAt = 1000, .firstAudioAt = 0, .acked = true, .ackAfterMs = 1500, .failAfterMs = 8000}) ==
        ReplyNudge::None);
  CHECK(replyNudge({.now = 9100, .userTurnAt = 1000, .firstAudioAt = 0, .acked = true, .ackAfterMs = 1500, .failAfterMs = 8000}) ==
        ReplyNudge::GiveUp);
  CHECK(replyNudge({.now = 9100, .userTurnAt = 1000, .firstAudioAt = 1800, .acked = true, .ackAfterMs = 1500, .failAfterMs = 8000}) ==
        ReplyNudge::None);

  CHECK(acknowledgementLine(VoiceLang::Es) == "Un momento.");
  CHECK(acknowledgementLine(VoiceLang::En) == "One moment.");
  CHECK(giveUpLine(VoiceLang::Es) == "No pude responder ahora; inténtalo de nuevo o sigue en la app.");
  CHECK(busyNotice(VoiceLang::Es, "Sala") ==
        "Te escuché en la cámara Sala, pero alguien la está usando; sigue en la app.");
  CHECK(cameraNote(VoiceLang::Es, "Sala").find("Sala") != std::string::npos);
  CHECK(mintConversationId().size() == 32);
  CHECK(conversationStateToString(ConversationState::Resumable) == "resumable");
  CHECK(conversationHandoffToString(ConversationHandoff::Bridging) == "bridging");
  CHECK(conversationAudienceToString(ConversationAudience::Discreet) == "discreet");
  CHECK(conversationEndToString(ConversationEnd::BusyLine) == "busy");
}

TEST_CASE("a dropped or busy camera hands the conversation to a free talking camera of the same room")
{
  const std::map<int64_t, WatchCameraInfo> cameras{
      {2, {.id = 2, .name = "Sala B", .talk = false, .panTilt = false, .tvExclusion = false}},
      {3, {.id = 3, .name = "Sala C", .talk = true, .panTilt = false, .tvExclusion = false}},
      {4, {.id = 4, .name = "Sala D", .talk = true, .panTilt = false, .tvExclusion = false}}};
  const auto info = [&cameras](int64_t id) -> std::optional<WatchCameraInfo> {
    const auto found = cameras.find(id);
    if (found == cameras.end())
      return std::nullopt;
    return found->second;
  };
  const std::vector<int64_t> mates{1, 2, 3, 4};
  const auto hop = valueOf(chooseHop({.from = 1,
                              .roommates = mates,
                              .cameraInfo = info,
                              .listening = [](int64_t) { return true; },
                              .occupied = [](int64_t id) { return id == 3; }}));
  CHECK(hop.cameraId == 4);
  CHECK(hop.name == "Sala D");
  CHECK_FALSE(chooseHop({.from = 1,
                         .roommates = mates,
                         .cameraInfo = info,
                         .listening = [](int64_t) { return false; },
                         .occupied = {}})
                  .has_value());
}

TEST_CASE("face first: one member, strangers, several members and no face give their own sets")
{
  const SeenFaceEvidence david{.personId = 1, .userId = 10, .name = "David", .role = "owner", .confidence = 0.9F,
                               .accepted = true, .visitor = false, .trusted = false};
  const SeenFaceEvidence maria{.personId = 2, .userId = 11, .name = "María", .role = "resident", .confidence = 0.8F,
                               .accepted = true, .visitor = false, .trusted = false};
  const SeenFaceEvidence stranger{.personId = 30, .userId = 0, .name = "", .role = "", .confidence = 0.7F,
                                  .accepted = true, .visitor = true, .trusted = false};
  SeenFaceEvidence blurry = david;
  blurry.accepted = false;

  const SpeakerResolution member = resolveSpeaker({.faces = {david}, .voice = std::nullopt, .lang = VoiceLang::Es});
  CHECK(member.audience == ConversationAudience::Member);
  CHECK(member.surface == ConversationSurface::Camera);
  CHECK(member.userId == 10);

  const SpeakerResolution discreet =
      resolveSpeaker({.faces = {david, stranger}, .voice = std::nullopt, .lang = VoiceLang::Es});
  CHECK(discreet.audience == ConversationAudience::Discreet);
  CHECK(discreet.userId == 10);
  CHECK(std::ranges::find(discreet.notes, discreetNote(VoiceLang::Es)) != discreet.notes.end());

  CHECK(resolveSpeaker({.faces = {david, maria}, .voice = std::nullopt, .lang = VoiceLang::Es}).audience ==
        ConversationAudience::Guest);
  const SpeakerResolution visitor = resolveSpeaker({.faces = {stranger}, .voice = std::nullopt, .lang = VoiceLang::Es});
  CHECK(visitor.audience == ConversationAudience::Visitor);
  CHECK(visitor.userId == 0);
  CHECK(visitor.strangerPersonId == 30);
  CHECK(resolveSpeaker({.faces = {blurry}, .voice = std::nullopt, .lang = VoiceLang::Es}).audience ==
        ConversationAudience::Guest);

  const SpeakerResolution voiceOnly = resolveSpeaker(
      {.faces = {}, .voice = HeardVoiceEvidence{.matched = true, .userId = 10, .name = "David", .score = 0.9F},
       .lang = VoiceLang::Es});
  CHECK(voiceOnly.audience == ConversationAudience::Guest);
  CHECK(voiceOnly.userId == 0);

  const SpeakerResolution disagree = resolveSpeaker(
      {.faces = {david}, .voice = HeardVoiceEvidence{.matched = true, .userId = 11, .name = "María", .score = 0.8F},
       .lang = VoiceLang::Es});
  CHECK(disagree.disagreement);
  CHECK(disagree.audience == ConversationAudience::Guest);
  CHECK(disagree.userId == 0);
  CHECK(std::ranges::find(member.notes, floorNote(VoiceLang::Es)) != member.notes.end());
}

TEST_CASE("the face window's answer keeps only the quality the matcher accepted")
{
  argus::camera::v1::FaceWindowResponse response;
  auto* good = response.add_faces();
  good->set_user_id(5);
  good->set_quality("accepted");
  auto* turned = response.add_faces();
  turned->set_user_id(6);
  turned->set_quality("turned");
  const std::vector<SeenFaceEvidence> faces = facesOf(response);
  REQUIRE(faces.size() == 2);
  CHECK(faces[0].accepted);
  CHECK_FALSE(faces[1].accepted);
  CHECK(delegationPhrase("Argus, puedes hablar con ella"));
  CHECK_FALSE(delegationPhrase("hablamos luego"));
}

namespace
{

ConversationSnapshot snapshotOf(ConversationAudience audience, ConversationState state)
{
  ConversationSnapshot snapshot;
  snapshot.conversationId = "0123456789abcdef0123456789abcdef";
  snapshot.userId = audience == ConversationAudience::Visitor ? 0 : 7;
  snapshot.cameraId = 3;
  snapshot.cameraName = "Sala";
  snapshot.state = state;
  snapshot.audience = audience;
  snapshot.startedAt = 100;
  snapshot.updatedAt = 120;
  snapshot.resumableUntil = state == ConversationState::Resumable ? 300 : 0;
  snapshot.turns = {{.id = 1, .role = "user", .text = "¿Está David?", .at = 101, .final = true},
                    {.id = 2, .role = "argus", .text = "Te dejo un mensaje.", .at = 102, .final = true}};
  return snapshot;
}

}

TEST_CASE("the live transcript frame carries the shape the app reads")
{
  ConversationSnapshot snapshot = snapshotOf(ConversationAudience::Member, ConversationState::Resumable);
  snapshot.endReason = ConversationEnd::CameraLost;
  const Json::Value info = watchUpdateInfo(snapshot);
  CHECK(info["conversationId"] == "0123456789abcdef0123456789abcdef");
  CHECK(info["cameraId"].asInt64() == 3);
  CHECK(info["cameraName"] == "Sala");
  CHECK(info["state"] == "resumable");
  CHECK(info["handoff"] == "camera");
  CHECK(info["audience"] == "member");
  CHECK(info["startedAt"].asInt64() == 100);
  CHECK(info["updatedAt"].asInt64() == 120);
  CHECK(info["resumableUntil"].asInt64() == 300);
  CHECK(info["endReason"] == "camera_lost");
  REQUIRE(info["turns"].size() == 2);
  CHECK(info["turns"][0]["role"] == "user");
  CHECK(info["turns"][1]["text"] == "Te dejo un mensaje.");
  CHECK(info["turns"][1]["final"].asBool());
  CHECK(info.getMemberNames().size() == 11);
  CHECK(watchUpdateInfo(snapshotOf(ConversationAudience::Member, ConversationState::Listening))["endReason"].isNull());

  const argus::voice::v1::WatchConversation wire = watchConversationOf(snapshot);
  CHECK(wire.state() == "resumable");
  CHECK(wire.end_reason() == "camera_lost");
  CHECK(wire.turns_size() == 2);
}

TEST_CASE("an ended camera conversation leaves the notices the plan names")
{
  const auto lost = watchNoticesFor({.snapshot = snapshotOf(ConversationAudience::Member, ConversationState::Resumable),
                                     .reason = ConversationEnd::CameraLost,
                                     .carry = std::nullopt},
                                    VoiceLang::Es, {});
  REQUIRE(lost.size() == 1);
  CHECK(lost.front().type == "watch_resume");
  CHECK(lost.front().data["conversationId"] == "0123456789abcdef0123456789abcdef");

  const auto busy = watchNoticesFor({.snapshot = snapshotOf(ConversationAudience::Member, ConversationState::Resumable),
                                     .reason = ConversationEnd::BusyLine,
                                     .carry = std::nullopt},
                                    VoiceLang::Es, {});
  REQUIRE(busy.size() == 1);
  CHECK(busy.front().type == "watch_busy");
  CHECK(busy.front().body == "Te escuché en la cámara Sala, pero alguien la está usando; sigue en la app.");

  const auto visitor = watchNoticesFor({.snapshot = snapshotOf(ConversationAudience::Visitor, ConversationState::Ended),
                                        .reason = ConversationEnd::Silence,
                                        .carry = std::nullopt},
                                       VoiceLang::Es, {1, 2});
  REQUIRE(visitor.size() == 2);
  CHECK(visitor.front().type == "watch_message");
  CHECK(visitor.front().userId == 1);
  CHECK(visitor.front().body == "¿Está David?");

  const auto discreet = watchNoticesFor({.snapshot = snapshotOf(ConversationAudience::Discreet, ConversationState::Ended),
                                         .reason = ConversationEnd::Silence,
                                         .carry = std::nullopt},
                                        VoiceLang::Es, {});
  REQUIRE(discreet.size() == 1);
  CHECK(discreet.front().type == "watch_details");
  CHECK(discreet.front().body == "Te dejo un mensaje.");
}

TEST_CASE("a carried conversation keeps its turns and drops the camera's notes")
{
  CallHistory camera(VoiceLang::Es);
  camera.addNote("Estás hablando por el altavoz de la cámara Sala.");
  camera.addUser("Argus, ¿qué tengo hoy?");
  camera.addAssistant("Tienes dos cosas.");
  CallHistory phone(VoiceLang::Es);
  phone.adoptTurns(camera.entries());
  CHECK(phone.userTurns() == 1);
  CHECK(phone.entries().back().message.content == "Tienes dos cosas.");
  CHECK(std::ranges::none_of(phone.entries(), [](const CallEntry& entry) { return entry.kind == CallEntryKind::Note; }));
}

TEST_CASE("a visitor's yes and no are rule-detected in Spanish, Peruvian Spanish and English")
{
  const auto answer = [](std::string_view text) { return consentAnswer({.text = text, .maxWords = 6, .wakeMaxDistance = 2}); };
  for (const std::string_view yes : {"sí", "Sí, claro", "ya pues", "Ya, ps", "dale", "normal nomás", "Sí, Argus",
                                     "sí, no hay problema", "de acuerdo, joven", "yes", "Sure.", "of course", "okay"})
  {
    CAPTURE(yes);
    CHECK(answer(yes) == ConsentAnswer::Yes);
  }
  for (const std::string_view no : {"no", "No, gracias", "mejor no", "nel", "nope", "no thanks", "sí... no",
                                    "prefiero que no", "I'd rather not"})
  {
    CAPTURE(no);
    CHECK(answer(no) == ConsentAnswer::No);
  }
  for (const std::string_view none : {"ya me voy", "¿está David?", "vengo a dejar un paquete para la señora", "Argus", "",
                                      "sí quiero dejarle un mensaje largo a la familia de la casa"})
  {
    CAPTURE(none);
    CHECK(answer(none) == ConsentAnswer::None);
  }
}

TEST_CASE("a visitor who says they are a minor is recognised by rule")
{
  for (const std::string_view minor : {"soy menor de edad", "Tengo 12 años", "tengo doce años", "I'm 12 years old",
                                       "I'm a kid", "soy una niña"})
  {
    CAPTURE(minor);
    CHECK(minorStatement(minor));
  }
  for (const std::string_view adult : {"tengo 25 años", "tengo dos hijos", "I'm 30 years old", "soy el cartero"})
  {
    CAPTURE(adult);
    CHECK_FALSE(minorStatement(adult));
  }
}

TEST_CASE("a visitor's consent moves only on a yes, a no or a minor, and the rest gets safety-only answers")
{
  const ConsentTurnInput base{.consent = VisitorConsent::Asked, .text = "", .asked = 1, .maxAsks = 2, .maxWords = 6,
                              .wakeMaxDistance = 2, .beforeQuestion = false, .lang = VoiceLang::Es};
  ConsentTurnInput input = base;
  input.text = "sí";
  const ConsentTurn yes = consentTurn(input);
  CHECK(yes.consent == VisitorConsent::Granted);
  CHECK_FALSE(yes.toAssistant);
  CHECK(yes.line == consentThanks(VoiceLang::Es));

  input = base;
  input.consent = VisitorConsent::Granted;
  input.text = "¿me puede decir la hora?";
  CHECK(consentTurn(input).toAssistant);

  input = base;
  input.text = "¿está David?";
  const ConsentTurn other = consentTurn(input);
  CHECK(other.consent == VisitorConsent::Asked);
  CHECK_FALSE(other.toAssistant);
  CHECK(other.askedAgain);
  CHECK(other.line.starts_with(std::string(safetyOnlyLine(VoiceLang::Es))));
  CHECK(other.line.find(std::string(consentAskAgain(VoiceLang::Es))) != std::string::npos);
  CHECK(other.line.find("David") == std::string::npos);
  input.asked = 2;
  const ConsentTurn silentAfter = consentTurn(input);
  CHECK_FALSE(silentAfter.askedAgain);
  CHECK(silentAfter.line == safetyOnlyLine(VoiceLang::Es));

  input = base;
  input.text = "no";
  const ConsentTurn no = consentTurn(input);
  CHECK(no.consent == VisitorConsent::Declined);
  CHECK_FALSE(no.toAssistant);
  input.consent = VisitorConsent::Declined;
  input.text = "bueno, sí";
  const ConsentTurn later = consentTurn(input);
  CHECK(later.consent == VisitorConsent::Declined);
  CHECK(later.line == safetyOnlyLine(VoiceLang::Es));

  input = base;
  input.text = "sí, tengo 12 años";
  const ConsentTurn minor = consentTurn(input);
  CHECK(minor.consent == VisitorConsent::Declined);
  CHECK(minor.minorStated);
  input.consent = VisitorConsent::Granted;
  input.text = "soy menor de edad";
  CHECK(consentTurn(input).consent == VisitorConsent::Declined);

  input = base;
  input.beforeQuestion = true;
  input.text = "Argus, ¿está David?";
  const ConsentTurn wake = consentTurn(input);
  CHECK(wake.consent == VisitorConsent::Asked);
  CHECK(wake.line.empty());
  CHECK_FALSE(wake.toAssistant);

  input = base;
  input.lang = VoiceLang::En;
  input.text = "yeah";
  CHECK(consentTurn(input).line == consentThanks(VoiceLang::En));
  CHECK(std::string(consentQuestion(VoiceLang::Es)).find("se guarda") != std::string::npos);
  CHECK(std::string(consentQuestion(VoiceLang::En)).find("kept") != std::string::npos);
  CHECK(visitorConsentToString(VisitorConsent::Asked) == "unanswered");
}

TEST_CASE("a household minor at a camera gets the minor set, and an unnamed household face is never a visitor")
{
  const SeenFaceEvidence child{.personId = 4, .userId = 12, .name = "Lucía", .role = "resident", .confidence = 0.9F,
                               .accepted = true, .visitor = false, .trusted = true};
  const SpeakerResolution member = resolveSpeaker({.faces = {child}, .voice = std::nullopt, .lang = VoiceLang::Es});
  const SpeakerResolution minor = asMinor(member, VoiceLang::Es);
  CHECK(minor.surface == ConversationSurface::CameraMinor);
  CHECK(minor.userId == 12);
  CHECK(std::ranges::find(minor.notes, minorNote(VoiceLang::Es)) != minor.notes.end());
  const SpeakerResolution guest = resolveSpeaker({.faces = {}, .voice = std::nullopt, .lang = VoiceLang::Es});
  CHECK(asMinor(guest, VoiceLang::Es).surface == guest.surface);

  const SeenFaceEvidence unnamed{.personId = 4, .userId = 0, .name = "", .role = "resident", .confidence = 0.9F,
                                 .accepted = true, .visitor = false, .trusted = true};
  CHECK(unnamedHousehold(unnamed));
  const SpeakerResolution hidden = resolveSpeaker({.faces = {unnamed}, .voice = std::nullopt, .lang = VoiceLang::Es});
  CHECK(hidden.audience == ConversationAudience::Guest);
  CHECK(hidden.userId == 0);
  const SeenFaceEvidence stranger{.personId = 30, .userId = 0, .name = "", .role = "", .confidence = 0.7F,
                                  .accepted = true, .visitor = true, .trusted = false};
  CHECK(resolveSpeaker({.faces = {unnamed, stranger}, .voice = std::nullopt, .lang = VoiceLang::Es}).audience ==
        ConversationAudience::Guest);
}

TEST_CASE("the visitor's message names the consent it was left under, and an alert names only the camera")
{
  ConversationSnapshot snapshot = snapshotOf(ConversationAudience::Visitor, ConversationState::Ended);
  snapshot.consent = VisitorConsent::Declined;
  const auto notices = watchNoticesFor({.snapshot = snapshot, .reason = ConversationEnd::Silence, .carry = std::nullopt},
                                       VoiceLang::Es, {1});
  REQUIRE(notices.size() == 1);
  CHECK(notices.front().data["visitorConsent"] == "declined");
  CHECK(cameraSpeaker(VoiceLang::Es, "Entrada") == "Alguien en la cámara Entrada");
  CHECK(cameraSpeaker(VoiceLang::En, "Gate") == "Someone at the Gate camera");
}

TEST_CASE("a member's reply to a visitor goes only to that camera, only from a notified member, and expires")
{
  VisitorReplies replies({.accept = std::chrono::minutes(30), .hold = std::chrono::minutes(10), .maxChars = 20});
  const int64_t until = replies.open(
      {.conversationId = "c1", .cameraId = 3, .personId = 30, .recipients = {1, 2}, .at = 1000});
  CHECK(until == 1000 + 1800);

  CHECK(replies.reply({.userId = 9, .conversationId = "c1", .text = "hola", .now = 1100}).outcome ==
        VisitorReplyOutcome::NotFound);
  CHECK(replies.reply({.userId = 1, .conversationId = "c2", .text = "hola", .now = 1100}).outcome ==
        VisitorReplyOutcome::NotFound);
  CHECK(replies.reply({.userId = 1, .conversationId = "c1", .text = "un mensaje demasiado largo para la cámara",
                       .now = 1100})
            .outcome == VisitorReplyOutcome::TooLong);
  CHECK(replies.reply({.userId = 1, .conversationId = "c1", .text = "ñññññññññññññññññññ", .now = 1100}).outcome ==
        VisitorReplyOutcome::Queued);
  const VisitorReplyResult queued = replies.reply({.userId = 2, .conversationId = "c1", .text = "Ya bajo", .now = 1200});
  CHECK(queued.outcome == VisitorReplyOutcome::Queued);
  CHECK(queued.cameraId == 3);
  CHECK(queued.replyUntil == 1200 + 600);
  CHECK(replies.pendingCount() == 1);

  CHECK_FALSE(replies.take({.cameraId = 4, .personId = 30, .now = 1300}).has_value());
  CHECK_FALSE(replies.take({.cameraId = 3, .personId = 31, .now = 1300}).has_value());
  const auto taken = replies.take({.cameraId = 3, .personId = 30, .now = 1300});
  REQUIRE(taken.has_value());
  CHECK(taken->text == "Ya bajo");
  CHECK_FALSE(replies.take({.cameraId = 3, .personId = 30, .now = 1300}).has_value());

  CHECK(replies.reply({.userId = 1, .conversationId = "c1", .text = "otra vez", .now = 1400}).outcome ==
        VisitorReplyOutcome::Queued);
  CHECK(replies.take({.cameraId = 3, .personId = 0, .now = 1400 + 601}) == std::nullopt);
  CHECK(replies.reply({.userId = 1, .conversationId = "c1", .text = "tarde", .now = 1000 + 1801}).outcome ==
        VisitorReplyOutcome::Expired);

  CHECK(visitorReplyLine(VoiceLang::Es, "Ya bajo") == "Tengo un mensaje de la casa para usted: Ya bajo");
  CHECK(visitorReplyLine(VoiceLang::En, "Coming") == "I have a message from the household for you: Coming");
  CHECK(utf8Characters("ñandú") == 5);
  CHECK(watchReplyOf({.outcome = VisitorReplyOutcome::Said, .replyUntil = 9, .cameraId = 3}).outcome() ==
        argus::voice::v1::WATCH_REPLY_OUTCOME_SAID);
  CHECK(watchReplyOf({.outcome = VisitorReplyOutcome::Expired, .replyUntil = 0, .cameraId = 3}).outcome() ==
        argus::voice::v1::WATCH_REPLY_OUTCOME_EXPIRED);
}
