#pragma once

#include <feature/llm/controllers/llm-controller.hxx>
#include <llm/llm-client.hxx>
#include <grpcpp/impl/service_type.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct LlmRpcInput
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
  std::function<argus::llm::Capabilities()> capabilities;
  std::function<LlmChatOutcome(const ChatRequest&)> chat;
  std::function<void(const LlmStreamInput&)> chatStream;
  int slots{1};
  std::vector<grpc::Service*> services;
};

class LlmRpcServer
{
public:
  explicit LlmRpcServer(LlmRpcInput input);
  ~LlmRpcServer();
  int port() const;
  void shutdown();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
