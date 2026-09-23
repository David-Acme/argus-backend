#pragma once

#include <monitor/health-event.hxx>

#include <memory>

class NatsBus;

class NatsHealthEventSink final : public IHealthEventSink
{
public:
  explicit NatsHealthEventSink(std::shared_ptr<NatsBus> bus);

  bool publish(const CameraHealthEvent& event) override;

private:
  std::shared_ptr<NatsBus> bus_;
};
