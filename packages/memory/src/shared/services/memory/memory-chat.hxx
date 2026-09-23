#pragma once

#include <llm/llm-service.hxx>
#include <string>

class IMemoryChat
{
public:
  virtual ~IMemoryChat() = default;

  virtual bool available() const = 0;
  virtual bool busy() const = 0;

  virtual std::string chat(const ChatRequest& request) const = 0;
};
