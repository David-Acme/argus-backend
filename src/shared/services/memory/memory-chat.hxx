#pragma once

#include <shared/services/llm/llm-service.hxx>
#include <string>

// Chat substrate of the memory workers (Ruling BZ): the legacy binds the
// in-process engine (in-process-memory-chat.hxx), the extracted service
// chats over the argus-llm wire and relies on the bounded work queue for
// back-pressure instead of isBusy polling.
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
