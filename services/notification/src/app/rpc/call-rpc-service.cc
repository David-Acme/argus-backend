#include "call-rpc-service.hxx"

#include <drogon/drogon.h>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

namespace
{
namespace v1 = argus::notification::v1;

grpc::ServerUnaryReactor* finishWith(grpc::CallbackServerContext* context,
                                     const grpc::Status& status)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(status);
  return reactor;
}

v1::CallClaimStatus toProto(CallClaimStatus status)
{
  switch (status) {
    case CallClaimStatus::Claimed:
      return v1::CALL_CLAIM_STATUS_CLAIMED;
    case CallClaimStatus::Taken:
      return v1::CALL_CLAIM_STATUS_TAKEN;
    case CallClaimStatus::Expired:
      return v1::CALL_CLAIM_STATUS_EXPIRED;
    case CallClaimStatus::NotFound:
      return v1::CALL_CLAIM_STATUS_NOT_FOUND;
  }
  return v1::CALL_CLAIM_STATUS_NOT_FOUND;
}

CallEndReport fromProto(v1::CallOutcome outcome)
{
  switch (outcome) {
    case v1::CALL_OUTCOME_DECLINED:
      return CallEndReport::Declined;
    case v1::CALL_OUTCOME_FAILED:
      return CallEndReport::Failed;
    default:
      return CallEndReport::Completed;
  }
}

void fillClaim(const CallClaimOutcome& outcome, v1::ClaimCallResponse* response)
{
  response->set_status(toProto(outcome.status));
  if (outcome.status != CallClaimStatus::Claimed || !outcome.call)
    return;
  const CallSchema& call = *outcome.call;
  const auto text = [&call](const char* key) {
    const Json::Value& value = call.data[key];
    return value.isString() ? value.asString() : std::string{};
  };
  const auto number = [&call](const char* key) -> int64_t {
    const Json::Value& value = call.data[key];
    return value.isIntegral() ? value.asInt64() : 0;
  };
  response->set_opening_line(outcome.openingLine);
  response->set_lang(call.lang);
  response->set_kind(std::string(callKindOf(call.trigger)));
  response->set_summary(call.summary);
  response->set_title(call.title);
  response->set_camera_id(number("cameraId"));
  response->set_camera_name(text("cameraName"));
  response->set_episode_id(number("episodeId"));
  response->set_environment_name(text("environmentName"));
  response->set_urgency(call.urgency);
}
}

CallRpcService::CallRpcService(std::shared_ptr<const CallEngine> engine,
                               NotificationCallCallers callers)
    : engine_(std::move(engine)), callers_(std::move(callers))
{
}

grpc::ServerUnaryReactor*
CallRpcService::ClaimCall(grpc::CallbackServerContext* context,
                          const v1::ClaimCallRequest* request,
                          v1::ClaimCallResponse* response)
{
  if (!argus::client::authorizeCaller(context, callers_.answer))
    return finishWith(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                            "call caller credential required"));
  if (request->user_id() <= 0 || request->call_id().empty())
    return finishWith(context,
                      grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                   "call_id and user_id are required"));
  auto* reactor = context->DefaultReactor();
  CallClaimRequest claim{.callId = request->call_id(),
                         .userId = request->user_id(),
                         .sessionId = request->session_id()};
  drogon::app().getLoop()->queueInLoop([this, reactor, response, claim]() {
    drogon::async_run([this, reactor, response,
                       claim]() -> drogon::Task<void> {
      try {
        fillClaim(co_await engine_->claim(claim), response);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Call RPC: ClaimCall failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL,
                                     "claim failed"));
      }
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor*
CallRpcService::EndCall(grpc::CallbackServerContext* context,
                        const v1::EndCallRequest* request,
                        v1::EndCallResponse* response)
{
  if (!argus::client::authorizeCaller(context, callers_.answer))
    return finishWith(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                            "call caller credential required"));
  auto* reactor = context->DefaultReactor();
  CallEndRequest end{.callId = request->call_id(),
                     .userId = request->user_id(),
                     .outcome = fromProto(request->outcome()),
                     .spoken = request->spoken()};
  drogon::app().getLoop()->queueInLoop([this, reactor, response, end]() {
    drogon::async_run([this, reactor, response, end]() -> drogon::Task<void> {
      try {
        response->set_ok(co_await engine_->end(end));
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Call RPC: EndCall failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "end failed"));
      }
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor*
CallRpcService::ScheduleCall(grpc::CallbackServerContext* context,
                             const v1::ScheduleCallRequest* request,
                             v1::ScheduleCallResponse* response)
{
  if (!argus::client::authorizeCaller(context, callers_.schedule))
    return finishWith(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                            "schedule caller credential required"));
  auto* reactor = context->DefaultReactor();
  CallScheduleRequest schedule{.userId = request->user_id(),
                               .fireAt = request->fire_at(),
                               .topic = request->topic(),
                               .lang = request->lang(),
                               .commandId = request->command_id()};
  drogon::app().getLoop()->queueInLoop([this, reactor, response, schedule]() {
    drogon::async_run([this, reactor, response,
                       schedule]() -> drogon::Task<void> {
      try {
        const CallScheduleOutcome outcome = co_await engine_->schedule(schedule);
        response->set_scheduled_id(outcome.scheduledId);
        response->set_duplicate(outcome.status == CallScheduleStatus::Duplicate);
        switch (outcome.status) {
          case CallScheduleStatus::Scheduled:
          case CallScheduleStatus::Duplicate:
            reactor->Finish(grpc::Status::OK);
            break;
          case CallScheduleStatus::Conflict:
            reactor->Finish(grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                                         outcome.reason));
            break;
          case CallScheduleStatus::TooMany:
            reactor->Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                         outcome.reason));
            break;
          case CallScheduleStatus::Invalid:
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         outcome.reason));
            break;
          case CallScheduleStatus::InPast:
          case CallScheduleStatus::TooFar:
            reactor->Finish(grpc::Status(grpc::StatusCode::OUT_OF_RANGE,
                                         outcome.reason));
            break;
        }
      }
      catch (const std::exception& error) {
        LOG_WARN << "Call RPC: ScheduleCall failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL,
                                     "schedule failed"));
      }
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor*
CallRpcService::AnnounceAgenda(grpc::CallbackServerContext* context,
                               const v1::AnnounceAgendaRequest* request,
                               v1::AnnounceAgendaResponse* response)
{
  if (!argus::client::authorizeCaller(context, callers_.agenda))
    return finishWith(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                            "agenda caller credential required"));
  if (request->command_id().empty() || request->user_ids_size() == 0 ||
      !json_util::isValid(request->data()))
    return finishWith(context,
                      grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                   "user_ids, data and command_id are required"));
  AgendaAnnouncement announcement{.userIds = {request->user_ids().begin(),
                                              request->user_ids().end()},
                                  .leadMinutes = request->lead_minutes(),
                                  .title = request->title(),
                                  .body = request->body(),
                                  .data = json_util::fromString(request->data()),
                                  .commandId = request->command_id()};
  auto* reactor = context->DefaultReactor();
  drogon::app().getLoop()->queueInLoop([this, reactor, response,
                                        announcement]() {
    drogon::async_run([this, reactor, response,
                       announcement]() -> drogon::Task<void> {
      try {
        const auto outcome = co_await engine_->announceAgenda(announcement);
        response->set_notified(outcome.notified);
        response->set_rang(outcome.rang);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Call RPC: AnnounceAgenda failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL,
                                     "agenda announcement failed"));
      }
    });
  });
  return reactor;
}
