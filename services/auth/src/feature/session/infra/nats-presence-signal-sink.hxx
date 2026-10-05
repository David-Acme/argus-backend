#pragma once

#include <feature/session/services/presence-signal.hxx>
#include <memory>

class NatsBus;

class NatsPresenceSignalSink final : public PresenceSignalSink
{
public:
  explicit NatsPresenceSignalSink(std::shared_ptr<NatsBus> bus);

  void publish(const PresenceSignal& signal) override;

private:
  std::shared_ptr<NatsBus> bus_;
};
