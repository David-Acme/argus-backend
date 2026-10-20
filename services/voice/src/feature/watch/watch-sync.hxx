#pragma once

#include <feature/watch/camera-conversation.hxx>
#include <feature/watch/conversation-policy.hxx>
#include <feature/watch/visitor-replies.hxx>

#include <json/value.h>
#include <identity/identity-client.hxx>
#include <notification/notification-client.hxx>
#include <sync/sync-client.hxx>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

[[nodiscard]] Json::Value watchUpdateInfo(const ConversationSnapshot& snapshot);

struct WatchNotice
{
  int64_t userId{0};
  std::string type;
  std::string title;
  std::string body;
  Json::Value data;
};

[[nodiscard]] std::vector<WatchNotice> watchNoticesFor(const ConversationEnded& ended, VoiceLang lang,
                                                      const std::vector<int64_t>& owners);

struct WatchPublisherInput
{
  std::shared_ptr<const SyncClient> sync;
  std::shared_ptr<const NotificationClient> notifications;
  std::shared_ptr<const IdentityClient> identity;
  VoiceLang lang{VoiceLang::Es};
  std::shared_ptr<VisitorReplies> replies;
};

class WatchPublisher
{
public:
  explicit WatchPublisher(WatchPublisherInput input) : input_(std::move(input)) {}

  void update(const ConversationSnapshot& snapshot) const;
  void ended(const ConversationEnded& ended) const;

private:
  [[nodiscard]] std::vector<int64_t> owners() const;
  void notify(const WatchNotice& notice) const;

  WatchPublisherInput input_;
};
