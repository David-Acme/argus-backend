#pragma once

#include <shared/services/llm/llm-service.hxx>
#include <shared/services/memory/memory-chat.hxx>
#include <string>

// The memory worker's chat straight into the host engine — the brain hosts
// the memory stack in process, so worker generations share the LlmService
// the tool loop rides.
class InProcessMemoryChat final : public IMemoryChat
{
public:
  explicit InProcessMemoryChat(LlmService& llm) : llm_(llm) {}

  bool available() const override { return llm_.isLoaded(); }
  bool busy() const override { return llm_.isBusy(); }
  std::string chat(const ChatRequest& request) const override
  {
    return llm_.chat(request);
  }

private:
  LlmService& llm_;
};
