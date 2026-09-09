#pragma once

#include <memory>
#include <shared/contracts/identity-change-sink.hxx>

class NatsBus;

// NATS substrate of the identity-domain change events; the gateway's sync fan-out ignores the subject.
class NatsIdentityChangeSink : public IdentityChangeSink
{
public:
  explicit NatsIdentityChangeSink(std::shared_ptr<NatsBus> bus);

  void publish(const IdentityChangeInput& input) const override;

private:
  std::shared_ptr<NatsBus> bus_;
};
