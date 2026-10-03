#pragma once

#include <json/value.h>
#include <llm/llm-service.hxx>
#include <validation/validation_dsl.hxx>
#include <optional>
#include <string>
#include <vector>

struct ChatMessageDto
{
  std::string role;
  std::string content;
};

struct ChatCompletionDto
{
  std::vector<ChatMessageDto> messages;
  std::optional<int32_t> maxTokens;
  std::optional<float> temperature;
  bool resetContext{false};
  bool toolsEnabled{true};
  std::optional<int64_t> userId;
  std::string grammar;
  bool grammarRequired{false};
  std::optional<std::string> role;
  std::optional<std::string> lang;
  std::string sessionId;
  bool prefillOnly{false};

  static ChatCompletionDto fromJson(const Json::Value& json);

  ChatRequest request() const;
};
