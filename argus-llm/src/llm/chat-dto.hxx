#pragma once

#include <json/value.h>
#include <shared/services/llm/llm-service.hxx>
#include <shared/validation/validation_dsl.hxx>
#include <optional>
#include <string>
#include <vector>

// One chat message on the internal wire (Ruling BT): {"role", "content"}.
struct ChatMessageDto
{
  std::string role;
  std::string content;
};

// Internal wire request (Ruling BT): {messages, max_tokens?, temperature?,
// reset_context?}. Optional fields keep the engine's own defaults when
// absent (max_tokens 0 / temperature < 0 in ChatRequest semantics).
struct ChatCompletionDto
{
  std::vector<ChatMessageDto> messages;
  std::optional<int32_t> maxTokens;
  std::optional<float> temperature;
  bool resetContext{false};

  static ChatCompletionDto fromJson(const Json::Value& json);

  ChatRequest request() const;
};
