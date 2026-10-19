#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <llm/conversation-leg.hxx>
#include <notification/message-direction.hxx>
#include <notification/message-kind.hxx>
#include <notification/message-state.hxx>
#include <optional>
#include <string>

struct ConversationMessageSchema
{
  int64_t id{0};
  int64_t userId{0};
  int64_t counterpartId{0};
  std::string counterpartName;
  MessageDirection direction{MessageDirection::In};
  MessageKind kind{MessageKind::Turn};
  std::string body;
  std::optional<ConversationLegKind> leg;
  MessageState state{MessageState::Received};
  std::string messageKey;
  std::optional<std::string> replyToKey;
  bool isRead{false};
  std::optional<int64_t> readAt;
  int64_t expiresAt{0};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  ConversationMessageSchema() = default;
  explicit ConversationMessageSchema(const drogon::orm::Row& row);
  [[nodiscard]] Json::Value toJson() const;
};
