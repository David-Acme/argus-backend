#pragma once

#include <feature/vlm/services/vision-service.hxx>
#include <vlm/vlm-client.hxx>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct VlmRpcInput
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
  std::function<argus::vlm::Capabilities()> capabilities;
  std::function<std::string(const VisionDescribeMatInput&)> describe;
  int slots{1};
};

class VlmRpcServer
{
public:
  explicit VlmRpcServer(VlmRpcInput input);
  ~VlmRpcServer();
  int port() const;
  void shutdown();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
