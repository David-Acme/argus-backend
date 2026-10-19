#include "watch-confirm-notifier.hxx"

#include <notification/notification-client.hxx>

#include <json/writer.h>
#include <trantor/utils/Logger.h>

#include <random>
#include <string>
#include <utility>

namespace
{

constexpr const char* kType = "watch_confirm";
constexpr const char* kDevice = "argus-llm";

std::string commandIdOf()
{
  std::random_device entropy;
  return "watch-confirm-" + std::to_string(entropy()) + std::to_string(entropy());
}

}

CameraConfirmSink watchConfirmNotifier(std::shared_ptr<const NotificationClient> client)
{
  return [client = std::move(client)](const CameraConfirmRequest& confirm) {
    if (!client || confirm.userId <= 0)
      return;
    const bool english = confirm.lang == "en";
    Json::Value data(Json::objectValue);
    data["kind"] = kType;
    data["url"] = "/assistant/confirm?tool=" + confirm.tool;
    data["tool"] = confirm.tool;
    data["arguments"] = confirm.arguments;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    argus::notification::v1::CreateNotificationsRequest request;
    request.add_user_ids(confirm.userId);
    request.set_type(kType);
    request.set_title(english ? "Confirm in the app" : "Confirma en la app");
    request.set_body(english ? "You asked for this through a camera. Confirm it here."
                             : "Lo pediste por una cámara. Confírmalo aquí.");
    request.set_data(Json::writeString(builder, data));
    request.set_command_id(commandIdOf());
    const auto result =
        client->createNotifications(request, {.userId = 0, .role = "system", .device = std::string(kDevice)});
    if (result.outcome != NotificationRpcOutcome::Success)
      LOG_WARN << "argus-llm: the camera confirmation for " << confirm.tool
               << " was not created: " << result.status.error_message();
  };
}
