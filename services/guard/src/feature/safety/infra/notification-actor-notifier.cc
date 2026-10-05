#include "notification-actor-notifier.hxx"

#include <feature/safety/services/safety-copy.hxx>

#include <identity/identity-client.hxx>
#include <notification/notification-client.hxx>
#include <runtime/blocking-task.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <string>

NotificationActorNotifier::NotificationActorNotifier(Dependencies dependencies)
    : dependencies_(dependencies)
{
}

drogon::Task<bool> NotificationActorNotifier::confirmPanic(const SafetyAlertNotice& notice) const
{
  if (!dependencies_.notifications || notice.actorUserId <= 0)
    co_return false;
  const auto* identity = dependencies_.identity;
  const auto* notifications = dependencies_.notifications;
  const NotificationCreateResult result = co_await BlockingTask<NotificationCreateResult>{
      [identity, notifications, notice]() {
        std::string lang;
        if (identity) {
          if (const auto user = identity->getUser(notice.actorUserId);
              user && user->has_user())
            lang = user->user().lang();
        }
        const SafetyText text = safety_copy::panicSent(lang);
        Json::Value data(Json::objectValue);
        data["kind"] = "guard_panic_sent";
        data["urgency"] = "passive";
        data["silent"] = true;
        data["threadKey"] = "guard:panic:" + std::to_string(notice.alertId);
        data["alertId"] = static_cast<Json::Int64>(notice.alertId);
        data["environmentId"] = static_cast<Json::Int64>(notice.environmentId);
        data["cameraId"] = 0;
        data["lang"] = lang.empty() ? "es" : lang;
        argus::notification::v1::CreateNotificationsRequest request;
        request.add_user_ids(notice.actorUserId);
        request.set_command_id("panic:" + std::to_string(notice.alertId) + ":sent");
        request.set_type("camera");
        request.set_title(text.title);
        request.set_body(text.body);
        request.set_data(json_util::toString(data));
        return notifications->createNotifications(
            request, {.userId = 0, .role = "system", .device = "argus-guard"});
      }};
  if (result.outcome != NotificationRpcOutcome::Success) {
    LOG_WARN << "Guard safety: the panic confirmation for alert " << notice.alertId
             << " was not delivered";
    co_return false;
  }
  co_return true;
}
