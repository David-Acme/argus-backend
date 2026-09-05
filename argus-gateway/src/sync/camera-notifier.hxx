#pragma once

#include <json/value.h>
#include <shared/services/notification/notification-service.hxx>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class NatsBus;

// Gateway-side notification budget (Ruling AD): per camera and rolling hour,
// a cumulative digest when the budget is exceeded, and silent hours. Pure
// state — unit-testable without NATS or a database.
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

  // A one-line cumulative digest for the suppressed events, taken only when
  // the hour rolled over or the silent window ended — never during silent
  // hours (the counts carry until the next active window); resets the
  // counters. The roll inside shouldNotify marks the digest due instead of
  // clearing it, so no event arriving around the roll can lose the digest.
  std::string takeDigest(int64_t cameraId, int64_t nowMs);

  static bool inSilentHours(const Config& config, int hour);

private:
  struct CameraWindow
  {
    int64_t windowStartMs{0};
    int notified{0};
    // Set when the window rolled with suppressed counts still pending, so
    // the digest survives the roll until takeDigest flushes it.
    bool digestDue{false};
    std::map<std::string, int> suppressedByClass;
  };

  Config config_;
  std::map<int64_t, CameraWindow> windows_;
};

// Consumes argus.camera.v1.object_detected, applies the policy above and
// delivers to owner/guard users through the existing NotificationService.
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

// Subscribes the object_detected subject; events marshal into the Drogon
// loop before touching the policy or the database.
void subscribeObjectDetected(NatsBus& bus);
} // namespace camera_notifier
