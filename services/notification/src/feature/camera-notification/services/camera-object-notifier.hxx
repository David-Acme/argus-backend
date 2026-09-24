#pragma once

#include <feature/camera-notification/repositories/camera-fallback-log/camera-fallback-log-repository.hxx>
#include <feature/camera-notification/services/camera-notification-policy.hxx>
#include <identity/identity-client.hxx>
#include <json/value.h>
#include <shared/services/notification/notification-service.hxx>

#include <cstdint>
#include <memory>
#include <string>

class NatsBus;

struct CameraNotifierDependencies
{
  std::shared_ptr<IdentityClient> identityClient;
  NotificationService::Dependencies delivery;
};

class CameraObjectNotifier
{
public:
  CameraObjectNotifier(CameraNotificationPolicy::Config config,
                       CameraNotifierDependencies dependencies);

  void handle(const Json::Value& json);

  void flushDigests();

  CameraNotificationPolicy& policy() { return policy_; }

private:
  struct DeliverInput
  {
    const Json::Value& json;
    const std::string& title;
    const std::string& body;
    const std::string& commandId;
  };

  void deliver(const DeliverInput& input);

  void logFallback(const CameraFallbackLogInput& input);

  void purgeFallbackLog(int64_t nowS);

  NotificationService notificationService_;
  std::shared_ptr<IdentityClient> identityClient_;
  CameraFallbackLogRepository fallbackLogRepository_;
  CameraNotificationPolicy policy_;
};

namespace camera_notifier
{
CameraNotificationPolicy::Config resolveConfig();

void subscribe(NatsBus& bus, CameraObjectNotifier& notifier);
}
