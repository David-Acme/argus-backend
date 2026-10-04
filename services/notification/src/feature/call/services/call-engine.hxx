#pragma once

#include <config/notification-config.hxx>
#include <drogon/utils/coroutine.h>
#include <feature/call/repositories/arrival-seen/arrival-seen-repository.hxx>
#include <feature/call/repositories/call-preference/call-preference-repository.hxx>
#include <feature/call/repositories/call/call-repository.hxx>
#include <feature/call/repositories/scheduled-call/scheduled-call-repository.hxx>
#include <feature/call/services/call-copy.hxx>
#include <feature/call/services/call-policy.hxx>
#include <feature/call/services/call-ports.hxx>
#include <feature/call/services/call-trigger-classifier.hxx>
#include <nats/push-intent-sink.hxx>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct CallLocalTime
{
  int hour{0};
  int weekday{0};
};

struct CallEngineDependencies
{
  std::shared_ptr<const CallSignal> signal;
  std::shared_ptr<const LiveCallAnnouncer> announcer;
  std::shared_ptr<const CallDirectory> directory;
  std::shared_ptr<const CallNotificationSink> notifier;
  std::shared_ptr<const push_intent::PushIntentSink> push;
  std::function<int64_t()> clock;
  std::function<CallLocalTime(int64_t)> localTime;
  bool blockingOffLoop{true};
};

struct CallRequest
{
  CallCandidate candidate;
  std::vector<int64_t> userIds;
  bool notificationExists{false};
};

enum class CallResolution : uint8_t
{
  Rang = 0,
  Queued,
  Injected,
  Notified,
  Dropped
};

std::string callResolutionToString(CallResolution resolution);

struct CallUserOutcome
{
  int64_t userId{0};
  CallResolution resolution{CallResolution::Dropped};
  std::string reason;
  int64_t callId{0};
};

enum class CallClaimStatus : uint8_t
{
  Claimed = 0,
  Taken,
  Expired,
  NotFound
};

struct CallClaimRequest
{
  std::string callId;
  int64_t userId{0};
  std::string sessionId;
};

struct CallClaimOutcome
{
  CallClaimStatus status{CallClaimStatus::NotFound};
  std::optional<CallSchema> call;
  std::string openingLine;
};

enum class CallEndReport : uint8_t
{
  Completed = 0,
  Declined,
  Failed
};

struct CallEndRequest
{
  std::string callId;
  int64_t userId{0};
  CallEndReport outcome{CallEndReport::Completed};
  bool spoken{false};
};

struct CallScheduleRequest
{
  int64_t userId{0};
  int64_t fireAt{0};
  std::string topic;
  std::string lang;
  std::string commandId;
};

enum class CallScheduleStatus : uint8_t
{
  Scheduled = 0,
  Duplicate,
  Conflict,
  Invalid,
  TooMany
};

struct CallScheduleOutcome
{
  CallScheduleStatus status{CallScheduleStatus::Invalid};
  int64_t scheduledId{0};
  std::string reason;
};

struct AgendaAnnouncement
{
  std::vector<int64_t> userIds;
  int leadMinutes{0};
  std::string title;
  std::string body;
  Json::Value data;
  std::string commandId;
};

struct AgendaAnnouncementOutcome
{
  int notified{0};
  int rang{0};
};

struct KnownSeenEvent
{
  int64_t personId{0};
  int64_t cameraId{0};
  std::string cameraName;
  int64_t environmentId{0};
  std::string environmentName;
  int64_t at{0};
};

struct CallSweepReport
{
  int64_t pushed{0};
  int64_t missed{0};
  int64_t fired{0};
  int64_t closed{0};
};

class CallEngine
{
public:
  CallEngine(CallEngineConfig config, CallEngineDependencies dependencies);

  drogon::Task<std::vector<CallUserOutcome>>
  consider(const CallRequest& request) const;

  drogon::Task<std::vector<CallUserOutcome>>
  considerNotification(const Json::Value& data,
                       const std::vector<int64_t>& userIds) const;

  drogon::Task<CallClaimOutcome> claim(const CallClaimRequest& request) const;

  drogon::Task<bool> end(const CallEndRequest& request) const;

  drogon::Task<CallScheduleOutcome>
  schedule(const CallScheduleRequest& request) const;

  drogon::Task<std::vector<CallUserOutcome>>
  arrival(const KnownSeenEvent& event) const;

  drogon::Task<AgendaAnnouncementOutcome>
  announceAgenda(const AgendaAnnouncement& announcement) const;

  drogon::Task<CallSweepReport> sweep() const;

  void reconfigure(const CallEngineConfig& config);

  [[nodiscard]] CallEngineConfig config() const;

private:
  struct UserContext
  {
    int64_t userId{0};
    CallRecipient recipient;
    CallPreferenceSchema preference;
  };

  struct ConsiderUserInput
  {
    const CallRequest& request;
    const UserContext& user;
    const CallEngineConfig& config;
    int64_t now{0};
  };

  struct MissedInput
  {
    const CallSchema& call;
    std::string cancelReason;
    int64_t now{0};
  };

  drogon::Task<CallUserOutcome> considerUser(const ConsiderUserInput& input) const;

  drogon::Task<void> settleMissed(const MissedInput& input) const;

  drogon::Task<CallRecipient> lookupRecipient(int64_t userId) const;

  drogon::Task<CallPerson> lookupPerson(int64_t personId) const;

  drogon::Task<std::optional<bool>>
  announce(const CallAnnouncement& announcement) const;

  drogon::Task<bool> emit(const CallSignalInput& input) const;

  void pushRing(const CallSchema& call) const;

  int64_t now() const;

  CallLocalTime localAt(int64_t at) const;

  std::string langFor(const std::string& preferred) const;

  mutable std::mutex configMutex_;
  CallEngineConfig config_;
  CallEngineDependencies dependencies_;
  CallRepository callRepository_;
  CallPreferenceRepository preferenceRepository_;
  ScheduledCallRepository scheduledRepository_;
  ArrivalSeenRepository arrivalRepository_;
};

namespace call_engine
{
Json::Value incomingInfo(const CallSchema& call);
}
