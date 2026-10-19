#pragma once

#include <cstdint>
#include <string>

namespace messenger
{

struct ConversationNote
{
  int64_t userId{0};
  std::string text;
  int64_t at{0};
};

class ConversationPort
{
public:
  ConversationPort() = default;
  virtual ~ConversationPort() = default;
  ConversationPort(const ConversationPort&) = delete;
  ConversationPort& operator=(const ConversationPort&) = delete;

  [[nodiscard]] virtual bool note(const ConversationNote& note) const = 0;
};

class NoConversation final : public ConversationPort
{
public:
  [[nodiscard]] bool note(const ConversationNote&) const override { return false; }
};

}
