#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/identity/v1/voiceprint.grpc.pb.h>
#include <array>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <identity/voiceprint-client.hxx>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace
{

constexpr const char* kFleetSecret = "fleet-secret";

namespace v1 = argus::identity::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

class ScriptedVoiceprintService final
    : public v1::VoiceprintService::CallbackService
{
public:
  [[nodiscard]] std::map<std::string, std::string> seen() const
  {
    const std::scoped_lock lock(mutex_);
    return seen_;
  }

  [[nodiscard]] v1::ObserveVoiceTurnRequest lastTurn() const
  {
    const std::scoped_lock lock(mutex_);
    return lastTurn_;
  }

  [[nodiscard]] std::string lastClosed() const
  {
    const std::scoped_lock lock(mutex_);
    return lastClosed_;
  }

  [[nodiscard]] int calls() const
  {
    const std::scoped_lock lock(mutex_);
    return calls_;
  }

  Reactor* Identify(Ctx* context, const v1::IdentifyVoiceRequest* request,
                    v1::IdentifyVoiceResponse* out) override
  {
    out->set_outcome(v1::VOICEPRINT_OK);
    out->set_matched(request->sample().sample_rate() == 16000);
    out->set_user_id(7);
    return answer(context);
  }

  Reactor* ObserveTurn(Ctx* context, const v1::ObserveVoiceTurnRequest* request,
                       v1::IdentifyVoiceResponse* out) override
  {
    {
      const std::scoped_lock lock(mutex_);
      lastTurn_ = *request;
    }
    out->set_outcome(v1::VOICEPRINT_OK);
    out->set_matched(true);
    out->set_user_id(request->user_id());
    return answer(context);
  }

  Reactor* CloseCall(Ctx* context, const v1::CloseVoiceCallRequest* request,
                     v1::CloseVoiceCallResponse* out) override
  {
    {
      const std::scoped_lock lock(mutex_);
      lastClosed_ = request->call_key();
    }
    out->set_closed(true);
    return answer(context);
  }

private:
  Reactor* answer(Ctx* context)
  {
    std::map<std::string, std::string> headers;
    for (const auto& [key, value] : context->client_metadata())
      headers.emplace(std::string(key.begin(), key.end()),
                      std::string(value.begin(), value.end()));
    {
      const std::scoped_lock lock(mutex_);
      seen_ = std::move(headers);
      ++calls_;
    }
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::string> seen_;
  v1::ObserveVoiceTurnRequest lastTurn_;
  std::string lastClosed_;
  int calls_{0};
};

struct RunningServer
{
  std::unique_ptr<grpc::Server> server;
  std::string target;
};

RunningServer startServer(ScriptedVoiceprintService& service)
{
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  return {.server = builder.BuildAndStart(),
          .target = "127.0.0.1:" + std::to_string(port)};
}

constexpr std::array<int16_t, 4> kClip{0, 1, -1, 0x1234};

}

TEST_CASE("a voiceprint call the client cannot make never opens a socket")
{
  ScriptedVoiceprintService service;
  auto running = startServer(service);
  REQUIRE(running.server);
  const VoiceprintClient client(
      {.target = running.target, .fleetSecret = kFleetSecret});
  const VoiceClipView clip{.samples = kClip, .sampleRate = 16000};
  const VoiceClipView silent{.samples = {}, .sampleRate = 16000};
  const VoiceClipView ultrasonic{.samples = kClip, .sampleRate = 96000};

  CHECK_FALSE(client.identify(ultrasonic).has_value());
  CHECK_FALSE(
      client.identifyWithin({.sample = clip, .timeoutMs = 0}).has_value());
  CHECK_FALSE(client
                  .observeTurn({.sample = silent,
                                .userId = 7,
                                .deviceHash = "phone",
                                .callKey = "call",
                                .timeoutMs = 800})
                  .has_value());
  CHECK_FALSE(client
                  .observeTurn({.sample = clip,
                                .userId = 0,
                                .deviceHash = "phone",
                                .callKey = "call",
                                .timeoutMs = 800})
                  .has_value());
  CHECK_FALSE(client
                  .observeTurn({.sample = clip,
                                .userId = 7,
                                .deviceHash = "",
                                .callKey = "call",
                                .timeoutMs = 800})
                  .has_value());
  CHECK_FALSE(client
                  .observeTurn({.sample = clip,
                                .userId = 7,
                                .deviceHash = "phone",
                                .callKey = std::string(129, 'k'),
                                .timeoutMs = 800})
                  .has_value());
  CHECK_FALSE(client.closeCall({.callKey = "", .timeoutMs = 500}));
  CHECK(service.calls() == 0);

  const auto found = client.identify(clip);
  if (!found) {
    FAIL("identify answered nothing");
    return;
  }
  CHECK(found->matched());
  CHECK(service.calls() == 1);
  CHECK(service.seen()["x-argus-fleet"] == kFleetSecret);

  running.server->Shutdown();
}

TEST_CASE("a call turn travels with its caller, device and call key")
{
  ScriptedVoiceprintService service;
  auto running = startServer(service);
  REQUIRE(running.server);
  const VoiceprintClient client(
      {.target = running.target, .fleetSecret = kFleetSecret});
  const VoiceClipView clip{.samples = kClip, .sampleRate = 22050};

  const auto answer = client.observeTurn({.sample = clip,
                                          .userId = 7,
                                          .deviceHash = "phone-hash",
                                          .callKey = "call-1",
                                          .timeoutMs = 800});
  if (!answer) {
    FAIL("observeTurn answered nothing");
    return;
  }
  CHECK(answer->user_id() == 7);
  const auto request = service.lastTurn();
  CHECK(request.user_id() == 7);
  CHECK(request.device_hash() == "phone-hash");
  CHECK(request.call_key() == "call-1");
  const std::string& bytes = request.sample().pcm16();
  REQUIRE(bytes.size() == kClip.size() * 2);
  CHECK(static_cast<unsigned char>(bytes[2]) == 0x01);
  CHECK(static_cast<unsigned char>(bytes[3]) == 0x00);
  CHECK(static_cast<unsigned char>(bytes[4]) == 0xFF);
  CHECK(static_cast<unsigned char>(bytes[5]) == 0xFF);
  CHECK(static_cast<unsigned char>(bytes[6]) == 0x34);
  CHECK(static_cast<unsigned char>(bytes[7]) == 0x12);
  CHECK(request.sample().sample_rate() == 22050);
  CHECK(service.seen()["x-argus-fleet"] == kFleetSecret);
  CHECK_FALSE(service.seen().contains("authorization"));

  CHECK(client.closeCall({.callKey = "call-1", .timeoutMs = 500}));
  CHECK(service.lastClosed() == "call-1");

  running.server->Shutdown();
}
