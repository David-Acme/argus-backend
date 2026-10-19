#pragma once

#include <drogon/utils/coroutine.h>
#include <auth/jwt-filter.hxx>
#include <json/value.h>
#include <optional>
#include <sync/sync-filter.hxx>
#include <vector>

struct ConversationPull
{
  bool requiredCreate{false};
  bool findLastCreated{false};
  SyncFilter created;
  bool requiredDeleted{false};
  bool findLastDeleted{false};
  SyncFilter deleted;
};

struct ConversationPage
{
  std::vector<Json::Value> created;
  std::optional<Json::Value> lastCreated;
  std::vector<Json::Value> deleted;
  std::optional<Json::Value> lastDeleted;
};

class NotificationSyncSource
{
public:
  virtual ~NotificationSyncSource() = default;

  [[nodiscard]] virtual drogon::Task<std::vector<Json::Value>>
  find(const JwtContext& ctx, const SyncFilter& filter) const = 0;

  [[nodiscard]] virtual drogon::Task<std::optional<Json::Value>>
  findLast(const JwtContext& ctx) const = 0;

  [[nodiscard]] virtual drogon::Task<ConversationPage>
  conversation(const JwtContext& ctx, const ConversationPull& pull) const = 0;
};
