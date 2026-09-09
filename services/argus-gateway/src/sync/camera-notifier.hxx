#pragma once

#include <json/value.h>
#include <shared/services/notification/notification-service.hxx>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class NatsBus;

// Notification budget (Ruling AD): rolling hour, digest, silent hours. Pure state.
class CameraNotificationPolicy
{
public:
  struct Config
  {
    int budgetPerHour{6};
    // Local hours; both set (-1 off) and wrapping (23 -> 7) supported.
    int silentStartHour{-1};
    int silentEndHour{-1};
  };

  explicit CameraNotificationPolicy(Config config);

  // Rolls stale windows; true when a notification may go out now.
  bool shouldNotify(int64_t cameraId, int64_t nowMs);

  void countSuppressed(int64_t cameraId, const std::string& objectClass);

  // Camera ids with tracked state (for the periodic digest flush).
  std::vector<int64_t> trackedCameras() const;

  // Digest for suppressed events, flushed only outside silent hours; resets counters.
  std::string takeDigest(int64_t cameraId, int64_t nowMs);

  static bool inSilentHours(const Config& config, int hour);

private:
  struct CameraWindow
  {
    int64_t windowStartMs{0};
    int notified{0};
    // Set on a roll with suppressed counts pending; takeDigest flushes it.
    bool digestDue{false};
    std::map<std::string, int> suppressedByClass;
  };

  Config config_;
  std::map<int64_t, CameraWindow> windows_;
};

// Applies the policy to object_detected and delivers through NotificationService.
class CameraObjectNotifier
{
public:
  explicit CameraObjectNotifier(CameraNotificationPolicy::Config config);

  // Runs on the Drogon loop (marshaled from the cnats dispatcher).
  void handle(const Json::Value& json);

  // Sends one digest per camera whose suppressed-events window closed.
  void flushDigests();

  CameraNotificationPolicy& policy() { return policy_; }

private:
  void deliver(const Json::Value& json,
               const std::string& title,
               const std::string& body);

  NotificationService notificationService_;
  CameraNotificationPolicy policy_;
};

namespace camera_notifier
{
// Resolves [notifications] keys: budget_per_hour, silent_start, silent_end.
CameraNotificationPolicy::Config resolveConfig();

// Subscribes object_detected; events marshal into the Drogon loop first.
void subscribeObjectDetected(NatsBus& bus);
} // namespace camera_notifier
