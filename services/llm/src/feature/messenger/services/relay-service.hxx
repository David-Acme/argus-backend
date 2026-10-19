#pragma once

#include <feature/messenger/repositories/relay-thread/relay-thread-repository.hxx>
#include <feature/messenger/repositories/visitor-notice/visitor-notice-repository.hxx>
#include <feature/messenger/services/conversation-port.hxx>
#include <feature/messenger/services/message-courier.hxx>
#include <feature/messenger/services/messenger-store.hxx>
#include <feature/messenger/services/relay-recipient.hxx>
#include <feature/messenger/vocabulary/messenger-settings.hxx>
#include <feature/messenger/vocabulary/relay-outcome.hxx>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

enum class RelayStep : std::uint8_t
{
  Sent,
  Ready,
  AskRecipient,
  NoRecipient
};

struct RelayRequest
{
  int64_t senderId{0};
  std::string senderName;
  std::string recipient;
  std::string message;
  std::string lang;
  bool afterQuestion{false};
  bool dryRun{false};
  std::vector<relay_recipient::Member> roster;
  int64_t now{0};
};

struct RelayResult
{
  RelayStep step{RelayStep::NoRecipient};
  RelayOutcome outcome{RelayOutcome::NoRecipient};
  RelayChannel channel{RelayChannel::Notification};
  std::vector<std::string> candidates;
  int64_t threadId{0};
  int64_t recipientId{0};
  std::string recipientName;
};

struct ReplyRequest
{
  int64_t replierId{0};
  std::string replierName;
  std::string message;
  std::string lang;
  std::string recipient;
  bool afterQuestion{false};
  bool dryRun{false};
  std::vector<relay_recipient::Member> roster;
  int64_t now{0};
};

struct VisitorRequest
{
  std::string recipient;
  std::string need;
  std::string camera;
  std::string lang;
  std::vector<relay_recipient::Member> roster;
  int64_t now{0};
};

struct VisitorNoticeKey
{
  std::string camera;
  int64_t recipientId{0};
  int64_t now{0};
};

struct SweepResult
{
  int threadsExpired{0};
  int threadsPurged{0};
  int noticesPurged{0};
  int settled{0};
};

struct VisitorReply
{
  bool needsAnswer{false};
  std::string line;
};

struct RelayServiceDependencies
{
  std::shared_ptr<const messenger::ConversationPort> conversation;
  std::shared_ptr<const messenger::MessageCourier> courier;
  std::shared_ptr<const MessengerStore> store;
  MessengerSettings settings;
};

class RelayService
{
public:
  explicit RelayService(RelayServiceDependencies dependencies);

  [[nodiscard]] RelayResult relay(const RelayRequest& request) const;
  [[nodiscard]] RelayResult reply(const ReplyRequest& request) const;
  [[nodiscard]] VisitorReply visitorAsk(const VisitorRequest& request) const;
  [[nodiscard]] SweepResult sweep(int64_t now) const;
  [[nodiscard]] int64_t reserveVisitorNotice(const VisitorNoticeKey& key) const;

private:
  struct DeliveryInput
  {
    int64_t recipientId{0};
    std::string recipientName;
    int64_t senderId{0};
    std::string senderName;
    std::string message;
    std::string lang;
    std::string replyToKey;
    bool dryRun{false};
    int64_t now{0};
  };

  struct VisitorDelivery
  {
    int64_t recipientId{0};
    std::string recipientName;
    std::string camera;
    std::string need;
    std::string lang;
    int64_t now{0};
  };

  void withDb(const std::function<void(sqlite3*)>& body) const;
  void admit(const std::string& message) const;
  [[nodiscard]] int64_t expiresAt(int64_t now) const;
  [[nodiscard]] std::string commandId(const std::string& prefix, int64_t now, int64_t recipientId) const;
  [[nodiscard]] RelayResult answerThreads(const std::vector<RelayThreadRow>& threads, const ReplyRequest& request) const;
  [[nodiscard]] RelayResult deliverTo(const DeliveryInput& input) const;
  void notifyVisitor(const VisitorDelivery& delivery) const;

  std::shared_ptr<const messenger::ConversationPort> conversation_;
  std::shared_ptr<const messenger::MessageCourier> courier_;
  std::shared_ptr<const MessengerStore> store_;
  mutable std::atomic<int64_t> sequence_{0};
  MessengerSettings settings_;
  RelayThreadRepository threadRepository_;
  VisitorNoticeRepository visitorNotices_;
};
