#pragma once

#include <drogon/utils/coroutine.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace argus::vlm { class Client; }

struct VlmDescribeInput
{
  std::string jpeg;
  std::string prompt;
  std::string cameraId;
};

struct VlmDescribeResult
{
  std::string caption;
};

class VlmHttpClient
{
public:
  explicit VlmHttpClient(std::string baseUrl, double timeoutS = 8.0);

  drogon::Task<std::optional<VlmDescribeResult>>
  describe(const VlmDescribeInput& input) const;

private:
  std::string baseUrl_;
  double timeoutS_;
};

class VlmClient
{
public:
  explicit VlmClient(std::string baseUrl, double timeoutS = 8.0);

  drogon::Task<std::optional<VlmDescribeResult>>
  describe(const VlmDescribeInput& input) const;

  bool remote() const;

private:
  struct RpcCache
  {
    std::string target;
    std::shared_ptr<argus::vlm::Client> client;
  };
  std::shared_ptr<argus::vlm::Client> rpcClient() const;

  std::string baseUrl_;
  double timeoutS_;
  mutable std::atomic<std::shared_ptr<RpcCache>> rpcCache_;
};
