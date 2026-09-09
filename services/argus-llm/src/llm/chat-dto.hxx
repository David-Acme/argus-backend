#pragma once

#include <json/value.h>
#include <shared/services/llm/llm-service.hxx>
#include <shared/validation/validation_dsl.hxx>
#include <optional>
#include <string>
#include <vector>

// One chat message on the internal wire: {"role", "content"}.
struct ChatMessageDto
{
  std::string role;
  std::string content;
};

// Internal wire request:
// {messages, max_tokens?, temperature?, reset_context?, user_id?}.
struct ChatCompletionDto
{
  std::vector<ChatMessageDto> messages;
  std::optional<int32_t> maxTokens;
  std::optional<float> temperature;
  bool resetContext{false};
  // Whose memory a tool call writes to and reads from (D4). Absent means the
  // unattributed caller: tools still run, scoped to user 0.
  std::optional<int64_t> userId;

  static ChatCompletionDto fromJson(const Json::Value& json);

  ChatRequest request() const;
};
