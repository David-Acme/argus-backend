#pragma once

#include <argus/guard/v1/duress.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct GuardDuressClientConfig
{
  std::string target;
  std::string credential;
};

enum class SpokenDuressReply : uint8_t
{
  Raised,
  Repeated,
  Stale,
  NotAllowed,
  Unavailable
};

struct SpokenDuressCall
{
  int64_t userId{0};
  std::string role;
  std::string key;
  int64_t saidAt{0};
};

struct CameraPanicCall
{
  int64_t cameraId{0};
  int64_t userId{0};
  std::string speaker;
  int64_t saidAt{0};
  std::string conversationId;
};

class GuardDuressClient
{
public:
  static constexpr int kCallTimeoutMs = 3000;

  explicit GuardDuressClient(GuardDuressClientConfig config);

  GuardDuressClient(const GuardDuressClient&) = delete;
  GuardDuressClient& operator=(const GuardDuressClient&) = delete;
  virtual ~GuardDuressClient() = default;

  [[nodiscard]] virtual std::optional<std::string> phraseKey() const;

  [[nodiscard]] virtual SpokenDuressReply raiseSpoken(const SpokenDuressCall& call) const;

  [[nodiscard]] virtual bool raiseCameraPanic(const CameraPanicCall& call) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::guard::v1::DuressService::StubInterface> stub_;
  std::string credential_;
};
