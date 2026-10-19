#pragma once

#include <feature/conversation/repositories/conversation-message/conversation-message-repository.hxx>
#include <feature/conversation/services/conversation-ports.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <llm/conversation-leg.hxx>
#include <memory>
#include <notification/message-direction.hxx>
#include <notification/message-kind.hxx>
#include <notification/message-state.hxx>
#include <optional>
#include <string>
#include <vector>

enum class RelayRoute : uint8_t
{
  Conversation = 0,
  Notification
};

struct RelayLetter
{
  int64_t fromUserId{0};
  std::string fromName;
  int64_t toUserId{0};
  std::string toName;
  MessageKind kind{MessageKind::Relay};
  std::string body;
  std::string spokenLine;
  std::string title;
  std::optional<std::string> replyToKey;
  std::string commandId;
  int64_t expiresAt{0};
};

struct RelayReceipt
{
  std::string messageKey;
  RelayRoute route{RelayRoute::Notification};
  bool duplicate{false};
};

struct SettleLetter
{
  std::string messageKey;
  MessageState state{MessageState::Expired};
};

struct TranscriptLine
{
  MessageDirection direction{MessageDirection::Out};
  std::string body;
};

struct TranscriptInput
{
  int64_t userId{0};
  std::optional<ConversationLegKind> leg;
  std::vector<TranscriptLine> lines;
  int64_t expiresAt{0};
  std::string commandId;
};

struct TranscriptOutcome
{
  int64_t appended{0};
  bool duplicate{false};
};

struct ConversationThreadRef
{
  int64_t userId{0};
  int64_t counterpartId{0};
};

class ConversationInboxService
{
public:
  static constexpr std::size_t kMaxBodyBytes = 4000;
  static constexpr std::size_t kMaxNameBytes = 120;
  static constexpr std::size_t kMaxKeyBytes = 160;
  static constexpr std::size_t kMaxTranscriptLines = 8;

  struct Dependencies
  {
    std::shared_ptr<const MessageAnnouncer> announcer;
    std::shared_ptr<const MessageNoticeWriter> notices;
  };

  ConversationInboxService() = default;
  explicit ConversationInboxService(Dependencies dependencies);

  [[nodiscard]] drogon::Task<RelayReceipt> relay(const RelayLetter& letter) const;

  [[nodiscard]] drogon::Task<int64_t> settle(const SettleLetter& letter) const;

  [[nodiscard]] drogon::Task<TranscriptOutcome> appendTranscript(const TranscriptInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<ConversationThreadSummary>> threads(int64_t userId) const;

  [[nodiscard]] drogon::Task<int64_t> markThreadRead(const ConversationThreadRef& ref) const;

  [[nodiscard]] drogon::Task<bool> removeThread(const ConversationThreadRef& ref) const;

  [[nodiscard]] drogon::Task<int64_t> expire(int64_t now) const;

private:
  [[nodiscard]] drogon::Task<RelayRoute> deliver(const RelayLetter& letter) const;
  [[nodiscard]] drogon::Task<void> moveOutgoing(const SettleLetter& letter) const;

  Dependencies dependencies_;
  ConversationMessageRepository repository_;
};

[[nodiscard]] bool validLetter(const RelayLetter& letter);

[[nodiscard]] bool validTranscript(const TranscriptInput& input);
