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

  [[nodiscard]] v1::EnrollVoiceprintRequest lastEnroll() const
  {
    const std::scoped_lock lock(mutex_);
    return lastEnroll_;
  }

  [[nodiscard]] int calls() const
  {
    const std::scoped_lock lock(mutex_);
    return calls_;
  }

  Reactor* CreateChallenge(Ctx* context,
                           const v1::CreateVoiceprintChallengeRequest* request,
                           v1::CreateVoiceprintChallengeResponse* out) override
  {
    out->set_outcome(v1::VOICEPRINT_OK);
    out->set_challenge_id("challenge");
    out->set_lang(request->has_lang() ? request->lang() : "es");
    return answer(context);
  }

  Reactor* Enroll(Ctx* context, const v1::EnrollVoiceprintRequest* request,
                  v1::EnrollVoiceprintResponse* out) override
  {
    {
      const std::scoped_lock lock(mutex_);
      lastEnroll_ = *request;
    }
    out->set_outcome(v1::VOICEPRINT_OK);
    out->mutable_status()->set_enrolled(true);
    out->mutable_status()->set_sample_count(request->samples_size());
    return answer(context);
  }

  Reactor* Verify(Ctx* context, const v1::VerifyVoiceprintRequest* request,
                  v1::VerifyVoiceprintResponse* out) override
  {
    out->set_outcome(v1::VOICEPRINT_OK);
    out->set_matched(request->user_id() == 7);
    out->set_score(0.8F);
    out->set_threshold(0.5F);
    return answer(context);
  }

  Reactor* Identify(Ctx* context, const v1::IdentifyVoiceRequest* request,
                    v1::IdentifyVoiceResponse* out) override
  {
    out->set_outcome(v1::VOICEPRINT_OK);
    out->set_matched(request->sample().sample_rate() == 16000);
    out->set_user_id(7);
    return answer(context);
  }

  Reactor* Delete(Ctx* context, const v1::DeleteVoiceprintRequest*,
                  v1::DeleteVoiceprintResponse* out) override
  {
    out->set_outcome(v1::VOICEPRINT_OK);
    out->set_deleted(true);
    return answer(context);
  }

  Reactor* GetStatus(Ctx* context, const v1::GetVoiceprintStatusRequest*,
                     v1::GetVoiceprintStatusResponse* out) override
  {
    out->set_outcome(v1::VOICEPRINT_OK);
    out->set_available(true);
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
  v1::EnrollVoiceprintRequest lastEnroll_;
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
  const VoiceprintSession session{.accessToken = "token", .deviceHash = ""};
  const VoiceprintSession anonymous{.accessToken = "", .deviceHash = ""};

  CHECK_FALSE(client.status(0).has_value());
  CHECK_FALSE(client.verify({.userId = 0, .sample = clip}).has_value());
  CHECK_FALSE(client.verify({.userId = 7, .sample = silent}).has_value());
  CHECK_FALSE(client.identify(ultrasonic).has_value());
  CHECK_FALSE(client.remove({.userId = 7, .session = anonymous}).has_value());
  CHECK_FALSE(
      client.createChallenge({.userId = 7, .lang = "es", .session = anonymous})
          .has_value());
  CHECK_FALSE(client
                  .enroll({.userId = 7,
                           .samples = {clip, silent},
                           .consent = true,
                           .consentVersion = "v1",
                           .challengeId = "challenge",
                           .faceImage = "",
                           .session = session})
                  .has_value());
  CHECK_FALSE(client
                  .enroll({.userId = 7,
                           .samples = {clip},
                           .consent = true,
                           .consentVersion = "v1",
                           .challengeId = "",
                           .faceImage = "",
                           .session = session})
                  .has_value());
  CHECK(service.calls() == 0);

  const auto status = client.status(7);
  REQUIRE(status);
  CHECK(status->available());
  CHECK(service.calls() == 1);

  running.server->Shutdown();
}

TEST_CASE("the gated calls carry the fleet secret, the bearer and the device")
{
  ScriptedVoiceprintService service;
  auto running = startServer(service);
  REQUIRE(running.server);
  const VoiceprintClient client(
      {.target = running.target, .fleetSecret = kFleetSecret});

  const auto removed = client.remove(
      {.userId = 7,
       .session = {.accessToken = "owner-token", .deviceHash = "phone-hash"}});
  REQUIRE(removed);
  CHECK(removed->deleted());
  auto presented = service.seen();
  CHECK(presented["x-argus-fleet"] == kFleetSecret);
  CHECK(presented["authorization"] == "Bearer owner-token");
  CHECK(presented["x-argus-device"] == "phone-hash");

  const VoiceClipView clip{.samples = kClip, .sampleRate = 16000};
  const auto verdict = client.verify({.userId = 7, .sample = clip});
  REQUIRE(verdict);
  CHECK(verdict->matched());
  presented = service.seen();
  CHECK(presented["x-argus-fleet"] == kFleetSecret);
  CHECK_FALSE(presented.contains("authorization"));

  running.server->Shutdown();
}

TEST_CASE("an enrollment travels as little-endian 16-bit samples")
{
  ScriptedVoiceprintService service;
  auto running = startServer(service);
  REQUIRE(running.server);
  const VoiceprintClient client(
      {.target = running.target, .fleetSecret = kFleetSecret});
  const VoiceClipView clip{.samples = kClip, .sampleRate = 22050};

  const auto enrolled =
      client.enroll({.userId = 7,
                     .samples = {clip, clip, clip},
                     .consent = true,
                     .consentVersion = "voiceprint-consent-v1",
                     .challengeId = "challenge",
                     .faceImage = "",
                     .session = {.accessToken = "token", .deviceHash = ""}});
  REQUIRE(enrolled);
  CHECK(enrolled->status().sample_count() == 3);

  const auto request = service.lastEnroll();
  REQUIRE(request.samples_size() == 3);
  const std::string& bytes = request.samples(0).pcm16();
  REQUIRE(bytes.size() == kClip.size() * 2);
  CHECK(static_cast<unsigned char>(bytes[2]) == 0x01);
  CHECK(static_cast<unsigned char>(bytes[3]) == 0x00);
  CHECK(static_cast<unsigned char>(bytes[4]) == 0xFF);
  CHECK(static_cast<unsigned char>(bytes[5]) == 0xFF);
  CHECK(static_cast<unsigned char>(bytes[6]) == 0x34);
  CHECK(static_cast<unsigned char>(bytes[7]) == 0x12);
  CHECK(request.samples(0).sample_rate() == 22050);
  CHECK(request.consent());
  CHECK(request.consent_version() == "voiceprint-consent-v1");
  CHECK_FALSE(request.has_face_image());

  running.server->Shutdown();
}
