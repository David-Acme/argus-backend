#pragma once

#include <feature/call/services/call-ports.hxx>

#include <memory>

class NatsBus;

class NatsCallPushSink final : public CallPushSink
{
public:
  explicit NatsCallPushSink(std::shared_ptr<NatsBus> bus);

  [[nodiscard]] bool publish(const PushSealedEnvelope& envelope) const override;

private:
  std::shared_ptr<NatsBus> bus_;
};
