#pragma once

#include <shared/services/llm/llm-service.hxx>
#include <shared/services/memory/memory-chat.hxx>
#include <string>

// Legacy substrate: the in-process engine, exactly the pre-cutover calls.
// Kept out of memory-chat.hxx so the extracted argus-memory binary never
// links the in-process engine wrapper.
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
