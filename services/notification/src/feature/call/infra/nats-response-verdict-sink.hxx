#pragma once

#include <feature/call/services/call-ports.hxx>

#include <memory>

class NatsBus;

class NatsResponseVerdictSink final : public ResponseVerdictSink
{
public:
  explicit NatsResponseVerdictSink(std::shared_ptr<NatsBus> bus);

  void publish(const ResponseVerdictEvent& event) const override;

private:
  std::shared_ptr<NatsBus> bus_;
};
