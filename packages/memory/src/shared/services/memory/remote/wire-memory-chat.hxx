#pragma once

#include <llm/llm-service.hxx>
#include <llm/llm-remote.hxx>
#include <shared/services/memory/memory-chat.hxx>
#include <string>
#include <utility>

class WireMemoryChat final : public IMemoryChat
{
public:
  WireMemoryChat(std::string baseUrl, int timeoutMs)
      : client_(std::move(baseUrl), timeoutMs)
  {
  }

  bool available() const override { return true; }
  bool busy() const override { return false; }
  std::string chat(const ChatRequest& request) const override
  {
    return client_.chat(request);
  }

private:
  LlmClient client_;
};
