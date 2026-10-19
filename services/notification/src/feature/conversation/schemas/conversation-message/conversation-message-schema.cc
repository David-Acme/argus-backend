#include "conversation-message-schema.hxx"

#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>

#include <cstdint>
#include <optional>

namespace
{
std::optional<int64_t> optionalInt(const drogon::orm::Field& field)
{
  if (field.isNull())
    return std::nullopt;
  return static_cast<int64_t>(field.as<long long>());
}

std::optional<std::string> optionalText(const drogon::orm::Field& field)
{
  if (field.isNull())
    return std::nullopt;
  return field.as<std::string>();
}

Json::Value jsonOf(const std::optional<int64_t>& value)
{
  return value ? Json::Value(static_cast<Json::Int64>(*value)) : Json::Value();
}

Json::Value jsonOf(const std::optional<std::string>& value)
{
  return value ? Json::Value(*value) : Json::Value();
}
}

ConversationMessageSchema::ConversationMessageSchema(const drogon::orm::Row& row)
    : id(static_cast<int64_t>(row["id"].as<long long>())),
      userId(static_cast<int64_t>(row["user_id"].as<long long>())),
      counterpartId(static_cast<int64_t>(row["counterpart_id"].as<long long>())),
      counterpartName(row["counterpart_name"].as<std::string>()),
      direction(messageDirectionFromString(row["direction"].as<std::string>()).value_or(MessageDirection::In)),
      kind(messageKindFromString(row["kind"].as<std::string>()).value_or(MessageKind::Turn)),
      body(row["body"].as<std::string>()),
      leg(row["leg"].isNull() ? std::nullopt : conversationLegKindFromString(row["leg"].as<std::string>())),
      state(messageStateFromString(row["state"].as<std::string>()).value_or(MessageState::Received)),
      messageKey(row["message_key"].as<std::string>()),
      replyToKey(optionalText(row["reply_to_key"])),
      isRead(row["is_read"].as<int>() != 0),
      readAt(optionalInt(row["read_at"])),
      expiresAt(static_cast<int64_t>(row["expires_at"].as<long long>())),
      createdAt(static_cast<int64_t>(row["created_at"].as<long long>())),
      updatedAt(optionalInt(row["updated_at"])),
      deletedAt(optionalInt(row["deleted_at"]))
{
}

Json::Value ConversationMessageSchema::toJson() const
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(id);
  json["userId"] = static_cast<Json::Int64>(userId);
  json["counterpartId"] = static_cast<Json::Int64>(counterpartId);
  json["counterpartName"] = counterpartName;
  json["direction"] = messageDirectionToString(direction);
  json["kind"] = messageKindToString(kind);
  json["body"] = body;
  json["leg"] = leg ? Json::Value(conversationLegKindToString(*leg)) : Json::Value();
  json["state"] = messageStateToString(state);
  json["messageKey"] = messageKey;
  json["replyToKey"] = jsonOf(replyToKey);
  json["isRead"] = isRead;
  json["readAt"] = jsonOf(readAt);
  json["createdAt"] = static_cast<Json::Int64>(createdAt);
  json["updatedAt"] = jsonOf(updatedAt);
  json["deletedAt"] = jsonOf(deletedAt);
  return json;
}
