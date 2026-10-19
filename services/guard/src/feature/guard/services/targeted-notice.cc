#include "targeted-notice.hxx"

#include <notification/notification-client.hxx>
#include <runtime/blocking-task.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <map>
#include <utility>

namespace targeted_notice
{
drogon::Task<bool> send(TargetedNoticeInput input)
{
  if (input.client == nullptr || input.users.empty())
    co_return false;
  std::map<std::string, std::vector<int64_t>> byLang;
  for (const ResponseUser& user : input.users)
    byLang[guard_copy::normalizeLang({.requested = user.lang, .fallback = "es"})].push_back(user.userId);
  bool delivered = true;
  for (const auto& [lang, userIds] : byLang) {
    const NoticeText text = guard_copy::render(input.notice, lang);
    Json::Value data = input.data.isObject() ? input.data : Json::Value(Json::objectValue);
    data["kind"] = noticeKindToString(input.notice.kind);
    data["urgency"] = guard_copy::urgency(input.notice);
    data["lang"] = lang;
    argus::notification::v1::CreateNotificationsRequest request;
    for (const int64_t userId : userIds)
      request.add_user_ids(userId);
    request.set_command_id(input.commandId + ":" + lang);
    request.set_type("camera");
    request.set_title(text.title);
    request.set_body(text.body);
    request.set_data(json_util::toString(data));
    NotificationClient* client = input.client;
    const NotificationCreateResult result = co_await BlockingTask<NotificationCreateResult>(
        [client, request]() {
          return client->createNotifications(request,
                                             {.userId = 0, .role = "system", .device = "argus-guard"});
        });
    if (result.outcome != NotificationRpcOutcome::Success) {
      LOG_WARN << "Guard notice " << input.commandId << " was not delivered to the " << lang << " batch";
      delivered = false;
    }
  }
  co_return delivered;
}
}
