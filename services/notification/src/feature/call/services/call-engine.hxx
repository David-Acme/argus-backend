#pragma once

#include <config/notification-config.hxx>
#include <drogon/utils/coroutine.h>
#include <feature/call/repositories/arrival-seen/arrival-seen-repository.hxx>
#include <feature/call/repositories/call-preference/call-preference-repository.hxx>
#include <feature/call/repositories/call-response/call-response-repository.hxx>
#include <feature/call/repositories/call/call-repository.hxx>
#include <feature/call/repositories/scheduled-call/scheduled-call-repository.hxx>
#include <feature/call/services/call-copy.hxx>
#include <feature/call/services/call-policy.hxx>
#include <feature/call/services/response-copy.hxx>
#include <feature/call/services/call-ports.hxx>
#include <feature/call/services/call-trigger-classifier.hxx>
#include <nats/push-intent-sink.hxx>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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
  std::shared_ptr<const ResponseVerdictSink> verdicts{};
  std::function<int64_t()> clock;
  std::function<CallLocalTime(int64_t)> localTime;
  bool blockingOffLoop{true};
};

struct CallRequest
{
  CallCandidate candidate;
  std::vector<int64_t> userIds;
  bool notificationExists{false};
  std::vector<CallResponseMember> members{};
};

struct ResponseRequest
{
  const Json::Value& data;
  const std::vector<int64_t>& userIds;
  const Json::Value& plan;
};

struct ResponseVerdictRequest
{
  int64_t responseId{0};
  int64_t userId{0};
  ResponseVerdict verdict{ResponseVerdict::FalseAlarm};
};

enum class ResponseVerdictStatus : uint8_t
{
  Recorded = 0,
  NotFound,
  Closed
};

struct ResponseVerdictOutcome
{
  ResponseVerdictStatus status{ResponseVerdictStatus::NotFound};
  Json::Value response;
};

struct ResponseViewRequest
{
  int64_t userId{0};
  int64_t responseId{0};
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
  int64_t escalated{0};
  int64_t expired{0};
  int64_t purged{0};
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

  drogon::Task<std::vector<CallUserOutcome>>
  respond(const ResponseRequest& request) const;

  drogon::Task<ResponseVerdictOutcome>
  verdict(const ResponseVerdictRequest& request) const;

  drogon::Task<Json::Value> responses(int64_t userId) const;

  drogon::Task<std::optional<Json::Value>>
  response(const ResponseViewRequest& request) const;

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
    const CallResponseMember* member{nullptr};
  };

  struct PreparedCall
  {
    CallUserOutcome outcome;
    bool settled{true};
    CallVerdict verdict;
    bool askedReminder{false};
    std::string lang;
    CallCopy copy;
    Json::Value stored;
    CallCreateInput row;
    std::optional<CallSchema> ringing;
  };

  struct FinishUserInput
  {
    const CallRequest& request;
    PreparedCall& prepared;
    std::optional<bool> heard;
    int64_t now{0};
  };

  struct ReachInput
  {
    const CallResponseSchema& response;
    std::vector<CallResponseMember> members;
    bool critical{false};
    ResponseReach reason{ResponseReach::NextStep};
    int64_t now{0};
  };

  struct CancelInput
  {
    const CallResponseSchema& response;
    int64_t exceptUserId{0};
    int64_t onlyUserId{0};
    std::string reason;
    std::string attendedBy;
    int64_t now{0};
  };

  struct PromptInput
  {
    const CallResponseSchema& response;
    bool confirmed{false};
    int64_t now{0};
  };

  struct MissedInput
  {
    const CallSchema& call;
    std::string cancelReason;
    int64_t now{0};
  };

  drogon::Task<PreparedCall> prepareUser(const ConsiderUserInput& input) const;

  drogon::Task<CallUserOutcome> finishUser(const FinishUserInput& input) const;

  drogon::Task<std::vector<std::optional<bool>>>
  announceAll(std::vector<CallAnnouncement> probes) const;

  drogon::Task<std::unordered_map<int64_t, CallRecipient>>
  lookupRecipients(std::vector<int64_t> userIds) const;

  drogon::Task<int64_t> purgeHistory(int64_t now) const;

  drogon::Task<void> settleMissed(const MissedInput& input) const;

  drogon::Task<void> reach(const ReachInput& input) const;

  drogon::Task<void> cancelRinging(const CancelInput& input) const;

  drogon::Task<void> promptContacts(const PromptInput& input) const;

  drogon::Task<void> emitResponse(int64_t responseId) const;

  drogon::Task<void> attendAnswered(const CallSchema& call) const;

  drogon::Task<int64_t> advanceResponses(int64_t now) const;

  drogon::Task<CallRecipient> lookupRecipient(int64_t userId) const;

  drogon::Task<CallPerson> lookupPerson(int64_t personId) const;

  drogon::Task<bool> emit(const CallSignalInput& input) const;

  void pushRing(const CallSchema& call) const;

  int64_t now() const;

  CallLocalTime localAt(int64_t at) const;

  std::string langFor(const std::string& preferred) const;

  mutable std::mutex configMutex_;
  mutable std::atomic<int64_t> nextPurgeAt_{0};
  CallEngineConfig config_;
  CallEngineDependencies dependencies_;
  CallRepository callRepository_;
  CallPreferenceRepository preferenceRepository_;
  ScheduledCallRepository scheduledRepository_;
  ArrivalSeenRepository arrivalRepository_;
  CallResponseRepository responseRepository_;
};

namespace call_engine
{
inline constexpr std::size_t kProbeParallel = 8;
inline constexpr int64_t kArrivalStaleS = 600;
inline constexpr int64_t kPurgeIntervalS = 3600;

Json::Value incomingInfo(const CallSchema& call);

struct AnnounceAllInput
{
  const LiveCallAnnouncer& announcer;
  const std::vector<CallAnnouncement>& probes;
  std::size_t parallel{1};
};

[[nodiscard]] std::vector<std::optional<bool>>
announceAll(const AnnounceAllInput& input);
}
