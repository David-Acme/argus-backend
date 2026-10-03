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
