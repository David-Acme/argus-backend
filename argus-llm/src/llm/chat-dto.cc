#include "chat-dto.hxx"

namespace
{

constexpr size_t kMaxMessages = 64;
constexpr size_t kMaxRoleLength = 32;
constexpr int kMaxTokensBound = 4096;
constexpr size_t kMaxMessageLength = 32 * 1024;

} // namespace

ChatCompletionDto ChatCompletionDto::fromJson(const Json::Value& json)
{
  ChatCompletionDto dto;
  for (const auto& entry : json["messages"]) {
    if (!entry.isObject())
      continue;
    ChatMessageDto message;
    message.role = entry.get("role", "").asString();
    message.content = entry.get("content", "").asString();
    dto.messages.push_back(std::move(message));
  }
  if (json.isMember("max_tokens") && json["max_tokens"].isInt())
    dto.maxTokens = json["max_tokens"].asInt();
  if (json.isMember("temperature") && json["temperature"].isNumeric())
    dto.temperature = json["temperature"].asFloat();
  if (json.isMember("reset_context") && json["reset_context"].isBool())
    dto.resetContext = json["reset_context"].asBool();

  START_VALIDATION(ChatCompletionDto, dto)
  ARRAY_NOT_EMPTY(messages, ChatMessageDto)
  MAX_ELEMENTS(messages, ChatMessageDto, kMaxMessages)
  CUSTOM_LAMBDA(messages, [](const ChatCompletionDto& value)
                    -> std::optional<std::string> {
    for (const auto& message : value.messages) {
      if (message.role.empty() || message.content.empty())
        return "every message needs a role and a content";
      if (message.role.size() > kMaxRoleLength)
        return "message role is too long";
      if (message.content.size() > kMaxMessageLength)
        return "message content is too long";
    }
    return std::nullopt;
  })
  CUSTOM_LAMBDA(maxTokens, [](const ChatCompletionDto& value)
                    -> std::optional<std::string> {
    if (value.maxTokens &&
        (*value.maxTokens < 1 || *value.maxTokens > kMaxTokensBound))
      return "max_tokens must be between 1 and 4096";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(temperature, [](const ChatCompletionDto& value)
                    -> std::optional<std::string> {
    if (value.temperature && (*value.temperature < 0.0F || *value.temperature > 2.0F))
      return "temperature must be between 0.0 and 2.0";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}

ChatRequest ChatCompletionDto::request() const
{
  ChatRequest req;
  req.messages.reserve(messages.size());
  for (const auto& message : messages)
    req.messages.push_back({message.role, message.content});
  req.maxTokens = maxTokens.value_or(0);
  req.temperature = temperature.value_or(-1.0F);
  req.resetContext = resetContext;
  return req;
}
