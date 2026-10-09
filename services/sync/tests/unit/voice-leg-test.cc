#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <cstdio>
#include <feature/transport/infra/voice-grpc-relay.hxx>
#include <fstream>
#include <json/value.h>
#include <string>
#include <text/json-util.hxx>
#include <vector>

TEST_CASE("voice gRPC config resolves the typed voice leg target")
{
  const char* path = "voice-leg-test-config.toml";
  {
    std::ofstream file(path);
    file << "[voice]\n"
         << "target = \"127.0.0.1:7034\"\n";
  }

  ConfigService::load(path);
  const VoiceGrpcConfig configured = VoiceGrpcConfig::resolve();
  CHECK(configured.target == "127.0.0.1:7034");

  {
    std::ofstream file(path);
    file << "[sync]\n"
         << "port = 7025\n";
  }

  ConfigService::load(path);
  const VoiceGrpcConfig fallback = VoiceGrpcConfig::resolve();
  CHECK(fallback.target.empty());

  std::remove(path);
}

TEST_CASE("renderServerFrame reproduces the frozen voice wire JSON")
{
  argus::voice::v1::ServerFrame stt;
  stt.mutable_stt()->set_text("hola argus");
  stt.mutable_stt()->set_final(true);
  const Json::Value sttJson = json_util::fromString(
      json_util::toString(VoiceGrpcRelay::renderServerFrame(stt)));
  CHECK(sttJson["type"] == "voice:stt");
  CHECK(sttJson["payload"]["text"] == "hola argus");
  CHECK(sttJson["payload"]["final"] == true);

  argus::voice::v1::ServerFrame assistant;
  assistant.mutable_assistant()->set_text("Hola de nuevo.");
  const Json::Value assistantJson = json_util::fromString(
      json_util::toString(VoiceGrpcRelay::renderServerFrame(assistant)));
  CHECK(assistantJson["type"] == "voice:assistant");
  CHECK(assistantJson["payload"]["text"] == "Hola de nuevo.");

  argus::voice::v1::ServerFrame event;
  event.mutable_event()->set_reaction(argus::voice::v1::REACTION_RECOGNIZING);
  event.mutable_event()->set_intensity(0.5F);
  event.mutable_event()->set_because("stt_failed");
  const Json::Value eventJson = json_util::fromString(
      json_util::toString(VoiceGrpcRelay::renderServerFrame(event)));
  CHECK(eventJson["type"] == "voice:event");
  CHECK(eventJson["payload"]["reaction"] == "recognizing");
  CHECK(eventJson["payload"]["intensity"].asDouble() == doctest::Approx(0.5));
  CHECK(eventJson["payload"]["because"] == "stt_failed");

  argus::voice::v1::ServerFrame done;
  done.mutable_done()->set_session_id(0);
  const Json::Value doneJson = json_util::fromString(
      json_util::toString(VoiceGrpcRelay::renderServerFrame(done)));
  CHECK(doneJson["type"] == "voice:done");
  CHECK(doneJson["payload"]["sessionId"].asInt64() == 0);
}

TEST_CASE("renderServerFrame renders the duplex turn frames")
{
  argus::voice::v1::ServerFrame turn;
  turn.mutable_turn()->set_id(3);
  const Json::Value turnJson = json_util::fromString(
      json_util::toString(VoiceGrpcRelay::renderServerFrame(turn)));
  CHECK(turnJson["type"] == "voice:turn");
  CHECK(turnJson["payload"]["id"].asInt64() == 3);

  argus::voice::v1::ServerFrame interrupted;
  interrupted.mutable_interrupted()->set_id(3);
  const Json::Value interruptedJson = json_util::fromString(
      json_util::toString(VoiceGrpcRelay::renderServerFrame(interrupted)));
  CHECK(interruptedJson["type"] == "voice:interrupted");
  CHECK(interruptedJson["payload"]["id"].asInt64() == 3);

  argus::voice::v1::ServerFrame assistant;
  assistant.mutable_assistant()->set_text("Hola.");
  assistant.mutable_assistant()->set_turn_id(3);
  const Json::Value assistantJson = json_util::fromString(
      json_util::toString(VoiceGrpcRelay::renderServerFrame(assistant)));
  CHECK(assistantJson["type"] == "voice:assistant");
  CHECK(assistantJson["payload"]["text"] == "Hola.");
  CHECK(assistantJson["payload"]["turnId"].asInt64() == 3);
}

TEST_CASE("A half-duplex assistant frame keeps the frozen payload")
{
  argus::voice::v1::ServerFrame assistant;
  assistant.mutable_assistant()->set_text("Hola.");
  const Json::Value assistantJson = json_util::fromString(
      json_util::toString(VoiceGrpcRelay::renderServerFrame(assistant)));
  CHECK(assistantJson["payload"].getMemberNames() ==
        std::vector<std::string>{"text"});
}

TEST_CASE("An unavailable assistant frame reaches the app with an empty text and the marker")
{
  argus::voice::v1::ServerFrame assistant;
  assistant.mutable_assistant()->set_speech("unavailable");
  const Json::Value assistantJson = json_util::fromString(
      json_util::toString(VoiceGrpcRelay::renderServerFrame(assistant)));
  CHECK(assistantJson["type"] == "voice:assistant");
  CHECK(assistantJson["payload"]["text"] == "");
  CHECK(assistantJson["payload"]["speech"] == "unavailable");
}

TEST_CASE("voice:start selects the duplex mode only when it asks for it")
{
  const auto modeOf = [](const std::string& raw) {
    return VoiceGrpcRelay::startModeOf(json_util::fromString(raw));
  };
  CHECK(modeOf(R"({"type":"voice:start","payload":{"mode":"duplex"}})") ==
        argus::voice::v1::VOICE_MODE_DUPLEX);
  CHECK(modeOf(R"({"type":"voice:start"})") ==
        argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
  CHECK(modeOf(R"({"type":"voice:start","payload":{}})") ==
        argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
  CHECK(modeOf(R"({"type":"voice:start","payload":{"mode":"half"}})") ==
        argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
  CHECK(modeOf(R"({"type":"voice:start","payload":{"mode":"DUPLEX"}})") ==
        argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
  CHECK(modeOf(R"({"type":"voice:start","payload":{"mode":1}})") ==
        argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
  CHECK(modeOf(R"({"type":"voice:start","payload":"duplex"})") ==
        argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
  CHECK(modeOf(R"({"type":"voice:start","payload":null})") ==
        argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
}

TEST_CASE("an app action reaches the app as voice:action with its arguments as an object")
{
  argus::voice::v1::ServerFrame action;
  action.mutable_action()->set_id(3);
  action.mutable_action()->set_name("app.set_guard_mode");
  action.mutable_action()->set_arguments(R"({"mode":"night"})");
  CHECK(json_util::toString(VoiceGrpcRelay::renderServerFrame(action)) ==
        R"({"payload":{"arguments":{"mode":"night"},"id":3,"name":"app.set_guard_mode"},"type":"voice:action"})");

  action.mutable_action()->set_arguments("not json");
  CHECK(VoiceGrpcRelay::renderServerFrame(action)["payload"]["arguments"] == Json::Value(Json::objectValue));
}

TEST_CASE("voice:context maps a camera event and a note, and bounds what it forwards")
{
  Json::Value camera(Json::objectValue);
  camera["kind"] = "cameraEvent";
  camera["camera"] = "Entrada";
  camera["text"] = "una persona en la puerta";
  const auto event = VoiceGrpcRelay::contextOf(camera);
  CHECK(event.kind() == argus::voice::v1::VOICE_CONTEXT_CAMERA_EVENT);
  CHECK(event.camera() == "Entrada");
  CHECK(event.text() == "una persona en la puerta");

  Json::Value note(Json::objectValue);
  note["kind"] = "anything";
  note["text"] = std::string(1000, 'x');
  const auto noted = VoiceGrpcRelay::contextOf(note);
  CHECK(noted.kind() == argus::voice::v1::VOICE_CONTEXT_NOTE);
  CHECK(noted.text().size() == 300);

  CHECK(VoiceGrpcRelay::contextOf(Json::Value("x")).text().empty());
}

TEST_CASE("voice:context keeps whole UTF-8 characters and gives the situation a larger bound")
{
  Json::Value note(Json::objectValue);
  note["kind"] = "note";
  note["text"] = std::string(299, 'x') + "á";
  CHECK(VoiceGrpcRelay::contextOf(note).text() == std::string(299, 'x'));

  Json::Value situation(Json::objectValue);
  situation["kind"] = "situation";
  situation["text"] = std::string(2000, 'y');
  const auto mapped = VoiceGrpcRelay::contextOf(situation);
  CHECK(mapped.kind() == argus::voice::v1::VOICE_CONTEXT_SITUATION);
  CHECK(mapped.text().size() == 900);
}

TEST_CASE("voice:action_result carries the id, the outcome and a bounded detail")
{
  Json::Value failed(Json::objectValue);
  failed["id"] = 4;
  failed["ok"] = false;
  failed["detail"] = std::string(400, 'z');
  const auto result = VoiceGrpcRelay::actionResultOf(failed);
  CHECK(result.id() == 4);
  CHECK_FALSE(result.ok());
  CHECK(result.detail().size() == 160);

  Json::Value done(Json::objectValue);
  done["id"] = "12";
  done["ok"] = true;
  const auto ok = VoiceGrpcRelay::actionResultOf(done);
  CHECK(ok.id() == 12);
  CHECK(ok.ok());

  Json::Value junk(Json::objectValue);
  junk["id"] = "12x";
  junk["ok"] = "yes";
  const auto rejected = VoiceGrpcRelay::actionResultOf(junk);
  CHECK(rejected.id() == 0);
  CHECK_FALSE(rejected.ok());
  CHECK(VoiceGrpcRelay::actionResultOf(Json::Value("x")).id() == 0);
}

TEST_CASE("voice:mute is on only when the payload says so")
{
  Json::Value muted(Json::objectValue);
  muted["muted"] = true;
  CHECK(VoiceGrpcRelay::mutedOf(muted));
  muted["muted"] = "true";
  CHECK_FALSE(VoiceGrpcRelay::mutedOf(muted));
  CHECK_FALSE(VoiceGrpcRelay::mutedOf(Json::Value(Json::objectValue)));
}

TEST_CASE("voice:start resumes a cut call only when it says so")
{
  CHECK(VoiceGrpcRelay::resumeOf(json_util::fromString(R"({"type":"voice:start","payload":{"mode":"duplex","resume":true}})")));
  CHECK_FALSE(VoiceGrpcRelay::resumeOf(json_util::fromString(R"({"type":"voice:start","payload":{"resume":"true"}})")));
  CHECK_FALSE(VoiceGrpcRelay::resumeOf(json_util::fromString(R"({"type":"voice:start"})")));
}
