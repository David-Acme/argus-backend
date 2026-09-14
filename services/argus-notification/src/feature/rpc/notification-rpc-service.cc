#include "notification-rpc-service.hxx"

#include <drogon/drogon.h>
#include <grpc-server-identity.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <vector>

namespace
{

NotificationSyncFilter
filterOf(const argus::notification::v1::NotificationPull& pull, int64_t userId)
{
  NotificationSyncFilter filter;
  filter.userId = userId;
  if (pull.has_created()) {
    const auto& range = pull.created();
    if (range.has_start_time())
      filter.startTime = range.start_time();
    if (range.has_start_id())
      filter.startId = range.start_id();
    if (range.has_end_time())
      filter.endTime = range.end_time();
  }
  return filter;
}

void toProto(const Json::Value& row,
             argus::notification::v1::NotificationRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_user_id(row["userId"].asInt64());
  out->set_type(row["type"].asString());
  out->set_title(row["title"].asString());
  out->set_body(row["body"].asString());
  out->set_data(json_util::toString(row["data"]));
  out->set_is_read(row["isRead"].asBool());
  if (!row["readAt"].isNull())
    out->set_read_at(row["readAt"].asInt64());
  out->set_created_at(row["createdAt"].asInt64());
}

} // namespace

NotificationRpcService::NotificationRpcService(Dependencies dependencies)
    : guardCallers_(
          {argus::sdk::CallerCredential{.service = "argus-guard",
                                        .secret = ConfigService::getString(
                                            "grpc.caller_guard")}}),
      gatewayCallers_(
          {argus::sdk::CallerCredential{.service = "argus-gateway",
                                        .secret = ConfigService::getString(
                                            "grpc.caller_gateway")}}),
      notificationService_(
          {.deliverySink = std::move(dependencies.deliverySink),
           .pushSink = std::move(dependencies.pushSink),
           .pushRequired = dependencies.pushRequired})
{
}

void NotificationRpcService::startDeliveryReconciler()
{
  drogon::app().getLoop()->runAfter(0.0, [this]() {
    drogon::async_run([this]() -> drogon::Task<void> {
      co_await notificationService_.deliverPending();
      co_return;
    });
  });
  drogon::app().getLoop()->runEvery(60.0, [this]() {
    drogon::async_run([this]() -> drogon::Task<void> {
      co_await notificationService_.deliverPending();
      co_return;
    });
  });
}

grpc::ServerUnaryReactor* NotificationRpcService::CreateNotifications(
    grpc::CallbackServerContext* context,
    const argus::notification::v1::CreateNotificationsRequest* request,
    argus::notification::v1::CreateNotificationsResponse* response)
{
  if (!argus::sdk::authorizeCaller(context, guardCallers_).has_value()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "argus-guard caller credential required"));
    return reactor;
  }
  if (request->command_id().empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "command_id is required"));
    return reactor;
  }
  if (!request->data().empty() && !json_util::isValid(request->data())) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "data must be valid JSON"));
    return reactor;
  }

  const argus::notification::v1::CreateNotificationsRequest create = *request;
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, create,
                                        responseWriter]() {
    drogon::async_run([this, reactor, create,
                       responseWriter]() -> drogon::Task<void> {
      try {
        NotificationBatchInput batch;
        batch.notification.type = create.type();
        batch.notification.title = create.title();
        batch.notification.body = create.body();
        batch.notification.data = json_util::fromString(create.data());
        batch.commandId = create.command_id();

        batch.userIds.reserve(create.user_ids_size());
        for (const int64_t userId : create.user_ids())
          batch.userIds.push_back(userId);

        const NotificationCreateOutcome outcome =
            co_await notificationService_.createManyAndEmit(batch);
        responseWriter->set_created(static_cast<int32_t>(outcome.createdCount));
        responseWriter->set_duplicate(outcome.duplicate);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const NotificationCommandConflict& e) {
        LOG_WARN << "Notification RPC: command conflict: " << e.what();
        reactor->Finish(
            grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what()));
      }
      catch (const NotificationValidationError& e) {
        reactor->Finish(
            grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what()));
      }
      catch (const std::exception& e) {
        LOG_WARN << "Notification RPC: CreateNotifications failed: "
                 << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* NotificationRpcService::PullNotifications(
    grpc::CallbackServerContext* context,
    const argus::notification::v1::PullNotificationsRequest* request,
    argus::notification::v1::PullNotificationsResponse* response)
{
  if (!argus::sdk::authorizeCaller(context, gatewayCallers_).has_value()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "argus-gateway caller credential required"));
    return reactor;
  }
  const auto userId = argus::sdk::callerUserId(context);
  if (!userId) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "identity metadata missing or invalid"));
    return reactor;
  }

  const argus::notification::v1::PullNotificationsRequest pull = *request;
  const int64_t sub = *userId;
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, pull, responseWriter,
                                        sub]() {
    drogon::async_run([this, reactor, pull, responseWriter,
                       sub]() -> drogon::Task<void> {
      try {
        const NotificationSyncFilter filter =
            filterOf(pull.notification(), sub);
        if (pull.notification().required_create()) {
          for (const auto& row : co_await repository_.findSync(filter))
            toProto(row, responseWriter->add_created());
        }
        if (pull.notification().find_last_created()) {
          const auto last = co_await repository_.findLastSync(filter);
          if (last)
            toProto(*last, responseWriter->mutable_last_created());
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Notification RPC: PullNotifications failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}
