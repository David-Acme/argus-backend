#pragma once

#include <shared/services/llm/llm-service.hxx>
#include <string>

// Chat substrate of the memory workers (Ruling BZ): the legacy binds the
// in-process engine, the extracted service chats over the argus-llm wire and
// relies on the bounded work queue for back-pressure instead of isBusy
// polling.
class IMemoryChat
{
public:
  virtual ~IMemoryChat() = default;

  // True when the engine can take a chat at all (model loaded / remote set).
  virtual bool available() const = 0;
  // True when the engine is busy right now; only the in-process engine
  // reports it, the wire leg is governed by the queue-depth gate.
  virtual bool busy() const = 0;

  virtual std::string chat(const ChatRequest& request) const = 0;
};

// Legacy substrate: the in-process engine, exactly the pre-cutover calls.
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
