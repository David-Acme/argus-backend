#pragma once

#include <feature/presence/services/presence-ports.hxx>

class NatsBus;

class NatsPresencePublisher final : public PresencePublisher
{
public:
  explicit NatsPresencePublisher(NatsBus* bus);

  void publish(const PresenceChange& change) override;

private:
  NatsBus* bus_{nullptr};
};
