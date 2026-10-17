#include "privacy-notice.hxx"

#include <identity/identity-client.hxx>
#include <notification/notification-client.hxx>
#include <runtime/blocking-task.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr const char* kEs = "es";
constexpr const char* kEn = "en";

std::string languageOf(const std::string& requested)
{
  return requested.rfind("en", 0) == 0 ? kEn : kEs;
}
}

namespace privacy_notice
{
namespace
{
const ICameraPrivacyNotifier* g_notifier = nullptr;
}

PrivacyNoticeText text(const PrivacyDoubtNotice& notice, std::string_view lang)
{
  if (languageOf(std::string(lang)) == kEn)
    return {.title = "Unconfirmed privacy mode",
            .body = "I cannot confirm whether the camera " + notice.cameraName +
                    " is still in privacy mode; until then I am not recording "
                    "it. Check it in the Tapo app."};
  return {.title = "Modo privacidad sin confirmar",
          .body = "No puedo confirmar si la cámara " + notice.cameraName +
                  " sigue en modo privacidad; mientras tanto no la grabo. "
                  "Revísala en la app Tapo."};
}

void setNotifier(const ICameraPrivacyNotifier* notifier)
{
  g_notifier = notifier;
}

const ICameraPrivacyNotifier* getNotifier()
{
  return g_notifier;
}

struct NotificationPrivacyNotifier::Impl
{
  explicit Impl(const PrivacyNoticeConfig& config)
      : identity(config.identityTarget,
                 argus::client::PeerCredential{.credential =
                                                   config.identityCredential,
                                               .fleetSecret =
                                                   config.identityFleetSecret}),
        notifications({.target = config.notificationTarget,
                       .credential = config.notificationCredential}),
        notificationTarget(config.notificationTarget)
  {
  }

  [[nodiscard]] std::map<std::string, std::vector<int64_t>> recipientsByLang()
      const
  {
    std::map<std::string, std::vector<int64_t>> grouped;
    const auto users = identity.listNotifiableUsers();
    if (!users)
      return grouped;
    for (const int64_t userId : *users)
    {
      if (userId <= 0)
        continue;
      std::string lang = kEs;
      if (const auto user = identity.getUser(userId);
          user && user->has_user())
        lang = languageOf(user->user().lang());
      grouped[lang].push_back(userId);
    }
    return grouped;
  }

  IdentityClient identity;
  NotificationClient notifications;
  std::string notificationTarget;
};
}

privacy_notice::NotificationPrivacyNotifier::NotificationPrivacyNotifier(
    const PrivacyNoticeConfig& config)
    : impl_(std::make_unique<Impl>(config))
{
}

privacy_notice::NotificationPrivacyNotifier::~NotificationPrivacyNotifier() =
    default;

drogon::Task<bool> privacy_notice::NotificationPrivacyNotifier::doubtful(
    const PrivacyDoubtNotice& notice) const
{
  if (impl_->notificationTarget.empty() || notice.cameraId <= 0)
    co_return false;
  const Impl* impl = impl_.get();
  const bool delivered = co_await BlockingTask<bool>([impl, notice]() {
    const auto grouped = impl->recipientsByLang();
    if (grouped.empty())
    {
      LOG_WARN << "Camera privacy: nobody could be told that camera "
               << notice.cameraId << " cannot be confirmed";
      return false;
    }
    bool any = false;
    for (const auto& [lang, users] : grouped)
    {
      if (users.empty())
        continue;
      const PrivacyNoticeText copy = text(notice, lang);
      Json::Value data(Json::objectValue);
      data["kind"] = "camera_privacy_coverage";
      data["urgency"] = "passive";
      data["silent"] = false;
      data["threadKey"] =
          "camera:privacy:" + std::to_string(notice.cameraId);
      data["cameraId"] = static_cast<Json::Int64>(notice.cameraId);
      data["lang"] = lang;
      argus::notification::v1::CreateNotificationsRequest request;
      for (const int64_t userId : users)
        request.add_user_ids(userId);
      request.set_command_id("camera:privacy:" +
                             std::to_string(notice.cameraId) +
                             ":unconfirmed:" + std::to_string(notice.atMs));
      request.set_type("camera");
      request.set_title(copy.title);
      request.set_body(copy.body);
      request.set_data(json_util::toString(data));
      const NotificationCreateResult result = impl->notifications.createNotifications(
          request, {.userId = 0, .role = "system", .device = "argus-camera"});
      if (result.outcome == NotificationRpcOutcome::Success || result.duplicate)
      {
        any = true;
        continue;
      }
      LOG_WARN << "Camera privacy: the notice for camera " << notice.cameraId
               << " was not delivered";
    }
    return any;
  });
  co_return delivered;
}
