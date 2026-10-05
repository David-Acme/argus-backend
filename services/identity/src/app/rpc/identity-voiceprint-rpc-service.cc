#include "identity-voiceprint-rpc-service.hxx"
#include "identity-callers.hxx"

#include <auth/user-role.hxx>
#include <ctime>
#include <drogon/drogon.h>
#include <grpc/grpc-server-identity.hxx>
#include <utility>

namespace
{

namespace v1 = argus::identity::v1;

constexpr size_t kMaxCallKeyLength = 128;
constexpr size_t kMaxDeviceHashLength = 256;

v1::VoiceprintOutcome wireOutcome(VoiceprintOutcome outcome)
{
  switch (outcome) {
    case VoiceprintOutcome::Ok:
      return v1::VOICEPRINT_OK;
    case VoiceprintOutcome::Unavailable:
      return v1::VOICEPRINT_UNAVAILABLE;
    case VoiceprintOutcome::SampleInvalid:
      return v1::VOICEPRINT_SAMPLE_INVALID;
    case VoiceprintOutcome::SampleTooShort:
      return v1::VOICEPRINT_SAMPLE_TOO_SHORT;
    case VoiceprintOutcome::SampleTooNoisy:
      return v1::VOICEPRINT_SAMPLE_TOO_NOISY;
    case VoiceprintOutcome::SampleClipped:
      return v1::VOICEPRINT_SAMPLE_CLIPPED;
  }
  return v1::VOICEPRINT_OUTCOME_UNSPECIFIED;
}

EncodedVoice pcmOf(const v1::VoiceClip& clip)
{
  return {.bytes = clip.pcm16(),
          .encoding = VoiceEncoding::Pcm16,
          .sampleRate = clip.sample_rate()};
}

int64_t nowSeconds()
{
  return static_cast<int64_t>(std::time(nullptr));
}

bool turnContextUsable(const v1::ObserveVoiceTurnRequest& request)
{
  return request.user_id() > 0 && !request.device_hash().empty() &&
         request.device_hash().size() <= kMaxDeviceHashLength &&
         !request.call_key().empty() &&
         request.call_key().size() <= kMaxCallKeyLength;
}

}

IdentityVoiceprintRpcService::IdentityVoiceprintRpcService(
    Dependencies dependencies)
    : dependencies_(std::move(dependencies)),
      service_(dependencies_.voiceprint), passive_(dependencies_.voiceprint)
{
}

grpc::ServerUnaryReactor* IdentityVoiceprintRpcService::refuseCaller(
    grpc::CallbackServerContext* context) const
{
  const auto admission =
      dependencies_.gate->admit(context, identity_callers::kVoiceprint);
  if (admission.admitted())
    return nullptr;
  return refuse(context,
                argus::client::FleetCallerGate::refusal(admission.verdict));
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::refuse(grpc::CallbackServerContext* context,
                                     const grpc::Status& status)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(status);
  return reactor;
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::dispatch(grpc::CallbackServerContext* context,
                                       Work work)
{
  auto* reactor = context->DefaultReactor();
  drogon::app().getLoop()->queueInLoop([reactor, work = std::move(work)]() {
    drogon::async_run([reactor, work]() -> drogon::Task<void> {
      try {
        reactor->Finish(co_await work());
      }
      catch (const std::exception& error) {
        LOG_WARN << "Identity voiceprint RPC failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL,
                                     "identity could not complete the call"));
      }
      co_return;
    });
  });
  return reactor;
}

drogon::Task<grpc::Status> IdentityVoiceprintRpcService::answerIdentify(
    const v1::VoiceClip& clip, v1::IdentifyVoiceResponse* response) const
{
  const auto result = co_await service_.identify(pcmOf(clip));
  response->set_outcome(wireOutcome(result.outcome));
  response->set_matched(result.matched);
  response->set_score(result.score);
  response->set_threshold(result.threshold);
  if (result.matched) {
    response->set_user_id(result.userId);
    response->set_name(result.name);
    if (result.role)
      response->set_role(userRoleToString(*result.role));
    if (result.personId)
      response->set_person_id(*result.personId);
  }
  co_return grpc::Status::OK;
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::Identify(grpc::CallbackServerContext* context,
                                       const v1::IdentifyVoiceRequest* request,
                                       v1::IdentifyVoiceResponse* response)
{
  if (auto* refused = refuseCaller(context))
    return refused;
  if (!request->has_sample())
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "sample is required"));
  return dispatch(context,
                  [this, request, response]() -> drogon::Task<grpc::Status> {
                    co_return co_await answerIdentify(request->sample(),
                                                      response);
                  });
}

grpc::ServerUnaryReactor* IdentityVoiceprintRpcService::ObserveTurn(
    grpc::CallbackServerContext* context,
    const v1::ObserveVoiceTurnRequest* request,
    v1::IdentifyVoiceResponse* response)
{
  if (auto* refused = refuseCaller(context))
    return refused;
  if (!request->has_sample())
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "sample is required"));
  std::optional<VoiceTurnInput> turn;
  if (turnContextUsable(*request))
    turn = VoiceTurnInput{.userId = request->user_id(),
                          .deviceHash = request->device_hash(),
                          .callKey = request->call_key(),
                          .sample = pcmOf(request->sample()),
                          .now = nowSeconds()};
  return dispatch(
      context,
      [this, request, response,
       turn = std::move(turn)]() mutable -> drogon::Task<grpc::Status> {
        const grpc::Status status =
            co_await answerIdentify(request->sample(), response);
        if (turn && admitLearning())
          drogon::async_run(
              [this, learn = std::move(*turn)]() mutable -> drogon::Task<void> {
                try {
                  co_await passive_.learnFromTurn(std::move(learn));
                }
                catch (const std::exception& error) {
                  LOG_WARN << "Voiceprint: a call turn was not learned: "
                           << error.what();
                }
                learning_.fetch_sub(1);
              });
        co_return status;
      });
}

grpc::ServerUnaryReactor*
IdentityVoiceprintRpcService::CloseCall(grpc::CallbackServerContext* context,
                                        const v1::CloseVoiceCallRequest* request,
                                        v1::CloseVoiceCallResponse* response)
{
  if (auto* refused = refuseCaller(context))
    return refused;
  if (request->call_key().empty() ||
      request->call_key().size() > kMaxCallKeyLength)
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "call_key is required"));
  return dispatch(context,
                  [this, request, response]() -> drogon::Task<grpc::Status> {
                    const auto outcome = co_await passive_.closeCall(
                        {.callKey = request->call_key(), .now = nowSeconds()});
                    response->set_closed(outcome != PassiveCallOutcome::NotFound);
                    co_return grpc::Status::OK;
                  });
}

bool IdentityVoiceprintRpcService::admitLearning() const
{
  if (learning_.fetch_add(1) < kMaxLearningInFlight)
    return true;
  learning_.fetch_sub(1);
  LOG_WARN << "Voiceprint: a call turn was skipped; " << kMaxLearningInFlight
           << " turns are already being learned";
  return false;
}

void IdentityVoiceprintRpcService::sweepIdleCalls() const
{
  drogon::async_run([this]() -> drogon::Task<void> {
    try {
      co_await passive_.expireCalls(nowSeconds());
    }
    catch (const std::exception& error) {
      LOG_WARN << "Voiceprint: idle calls could not be closed: "
               << error.what();
    }
  });
}

void IdentityVoiceprintRpcService::purgeExpiredSamples() const
{
  drogon::async_run([this]() -> drogon::Task<void> {
    try {
      co_await passive_.purgeExpired(nowSeconds());
    }
    catch (const std::exception& error) {
      LOG_WARN << "Voiceprint: expired learning samples could not be purged: "
               << error.what();
    }
  });
}
