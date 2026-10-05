#pragma once

#include <auth/user-role.hxx>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <sync/sync-operation.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct CallSignalInput
{
  int64_t userId{0};
  SyncOperation operation{SyncOperation::CallIncoming};
  Json::Value info;
};

class CallSignal
{
public:
  virtual ~CallSignal() = default;

  [[nodiscard]] virtual bool emit(const CallSignalInput& input) const = 0;
};

struct CallAnnouncement
{
  int64_t userId{0};
  std::string text;
  std::string kind;
  std::string callId;
};

class LiveCallAnnouncer
{
public:
  virtual ~LiveCallAnnouncer() = default;

  [[nodiscard]] virtual std::optional<bool> announce(const CallAnnouncement& input) const = 0;
};

struct CallRecipient
{
  bool found{false};
  std::string name;
  std::string lang;
  std::optional<UserRole> role;
  bool active{true};
};

struct CallPerson
{
  bool found{false};
  std::string name;
  int64_t userId{0};
};

class CallDirectory
{
public:
  virtual ~CallDirectory() = default;

  [[nodiscard]] virtual CallRecipient recipient(int64_t userId) const = 0;

  [[nodiscard]] virtual std::unordered_map<int64_t, CallRecipient>
  recipients(const std::vector<int64_t>& userIds) const
  {
    std::unordered_map<int64_t, CallRecipient> found;
    found.reserve(userIds.size());
    for (const int64_t userId : userIds)
      found.emplace(userId, recipient(userId));
    return found;
  }

  [[nodiscard]] virtual CallPerson person(int64_t personId) const = 0;
};

struct CallNotice
{
  int64_t userId{0};
  std::string type;
  std::string title;
  std::string body;
  Json::Value data;
  std::string commandId;
};

class CallNotificationSink
{
public:
  virtual ~CallNotificationSink() = default;

  virtual drogon::Task<bool> notify(const CallNotice& notice) const = 0;
};

struct ResponseVerdictEvent
{
  int64_t responseId{0};
  std::string kind;
  std::string threadKey;
  int64_t episodeId{0};
  int64_t environmentId{0};
  std::string verdict;
  int64_t userId{0};
  int64_t at{0};
};

class ResponseVerdictSink
{
public:
  virtual ~ResponseVerdictSink() = default;

  virtual void publish(const ResponseVerdictEvent& event) const = 0;
};
