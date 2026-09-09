#pragma once

#include <shared/services/llm/llm-service.hxx>
#include <string>

// Chat substrate of the memory workers: in-process engine or the argus-llm wire.
class IMemoryChat
{
public:
  virtual ~IMemoryChat() = default;

  // True when the engine can take a chat at all (model loaded / remote set).
  virtual bool available() const = 0;
  // True when the engine is busy right now; only the in-process engine reports it.
  virtual bool busy() const = 0;

  virtual std::string chat(const ChatRequest& request) const = 0;
};
