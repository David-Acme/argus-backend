#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <cstdio>
#include <feature/transport/infra/voice-grpc-relay.hxx>
#include <fstream>
#include <json/value.h>
#include <text/json-util.hxx>

// The voice leg's two frozen halves: which target it dials, and the JSON the
// app receives. Both moved here from the gateway's suite in sub-step 3a-1c,
// because the leg itself moved: the forwarder rides the /sync socket, and the
// socket is this service's.

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
