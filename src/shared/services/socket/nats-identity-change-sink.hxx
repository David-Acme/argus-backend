#pragma once

#include <memory>
#include <shared/contracts/identity-change-sink.hxx>

class NatsBus;

// NATS substrate of the identity-domain change events (Ruling BX): user and
// person writes funnel over `argus.identity.v1.change` for the memory catalog
// replicas. The gateway's wildcard sync fan-out ignores the subject; the
// gateway owns the /user fan-out natively.
class NatsIdentityChangeSink : public IdentityChangeSink
{
public:
  explicit NatsIdentityChangeSink(std::shared_ptr<NatsBus> bus);

  void publish(const IdentityChangeInput& input) const override;

private:
  std::shared_ptr<NatsBus> bus_;
};
