#include "notification-intent-notifier.hxx"

#include <json/writer.h>
#include <trantor/utils/Logger.h>

#include <utility>

namespace
{
constexpr const char* kType = "assistant_task";
constexpr const char* kKind = "assistant_task";
constexpr const char* kDevice = "argus-llm";
}

NotificationIntentNotifier::NotificationIntentNotifier(std::shared_ptr<const NotificationClient> client)
    : client_(std::move(client))
{
}

bool NotificationIntentNotifier::tell(const IntentNotice& notice) const
{
  if (!client_)
    return false;
  Json::Value data(Json::objectValue);
  data["kind"] = kKind;
  data["commandId"] = notice.commandId;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  argus::notification::v1::CreateNotificationsRequest request;
  request.add_user_ids(notice.userId);
  request.set_type(kType);
  request.set_title(notice.title);
  request.set_body(notice.body);
  request.set_data(Json::writeString(builder, data));
  request.set_command_id(notice.commandId);
  const auto result = client_->createNotifications(request, {.userId = 0, .role = "system", .device = std::string(kDevice)});
  if (result.outcome == NotificationRpcOutcome::Success)
    return true;
  LOG_WARN << "argus-llm: notice " << notice.commandId << " not created: " << result.status.error_message();
  return false;
}
