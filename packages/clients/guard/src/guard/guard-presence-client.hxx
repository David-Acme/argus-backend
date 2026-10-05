#pragma once

#include <argus/guard/v1/presence.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct GuardPresenceClientConfig
{
  std::string target;
  std::string credential;
};

class GuardPresenceClient
{
public:
  static constexpr int kCallTimeoutMs = 1500;

  explicit GuardPresenceClient(GuardPresenceClientConfig config);

  GuardPresenceClient(const GuardPresenceClient&) = delete;
  GuardPresenceClient& operator=(const GuardPresenceClient&) = delete;
  virtual ~GuardPresenceClient() = default;

  [[nodiscard]] virtual std::optional<argus::guard::v1::ListPresenceResponse>
  listPresence(const std::vector<int64_t>& userIds) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::guard::v1::PresenceService::StubInterface> stub_;
  std::string credential_;
};
