#pragma once

#include <feature/call/services/call-ports.hxx>

#include <atomic>
#include <memory>
#include <string>

class NatsBus;

namespace response_verdict_wire
{
std::string messageId(const ResponseVerdictEvent& event);
std::string payload(const ResponseVerdictEvent& event);
}

class NatsResponseVerdictSink final : public ResponseVerdictSink
{
public:
  explicit NatsResponseVerdictSink(std::shared_ptr<NatsBus> bus);

  void publish(const ResponseVerdictEvent& event) const override;

private:
  std::shared_ptr<NatsBus> bus_;
  std::shared_ptr<std::atomic<bool>> streamReady_;
};
