#pragma once

#include <llm/conversation-leg.hxx>

#include <cstdint>
#include <optional>
#include <string>

struct TranscriptExchange
{
  int64_t userId{0};
  std::optional<ConversationLegKind> leg;
  std::string userText;
  std::string reply;
  int64_t at{0};
};

class ConversationTranscript
{
public:
  ConversationTranscript() = default;
  virtual ~ConversationTranscript() = default;
  ConversationTranscript(const ConversationTranscript&) = delete;
  ConversationTranscript& operator=(const ConversationTranscript&) = delete;

  virtual void record(const TranscriptExchange& exchange) const = 0;
};

[[nodiscard]] bool transcribable(const TranscriptExchange& exchange);
