#pragma once

#include <atomic>
#include <cstdint>
#include <llm/llm-client.hxx>
#include <llm/llm-service.hxx>
#include <memory>
#include <string>

struct LlmRemoteConfig
{
  std::string url;
  int timeoutMs{120000};

  bool enabled() const { return !url.empty(); }

  static LlmRemoteConfig resolve();
};

class LlmHttpClient
{
public:
  LlmHttpClient(std::string baseUrl, int timeoutMs);

  LlmHttpClient& withCredential(std::string credential);

  std::string chat(const ChatRequest& request) const;

  void chatStream(const LlmStreamInput& input) const;

private:
  std::string chatBody(const ChatRequest& request) const;

  std::string baseUrl_;
  int timeoutMs_;
  std::string credential_;
};

class LlmClient
{
public:
  LlmClient(std::string baseUrl, int timeoutMs);

  std::string chat(const ChatRequest& request) const;

  void chatStream(const LlmStreamInput& input) const;

  bool remote() const;

private:
  struct RpcCache
  {
    std::string target;
    std::string credential;
    std::shared_ptr<argus::llm::Client> client;
  };

  std::shared_ptr<argus::llm::Client> rpcClient() const;

  std::string baseUrl_;
  int timeoutMs_;
  mutable std::atomic<std::shared_ptr<RpcCache>> rpcCache_;
};
