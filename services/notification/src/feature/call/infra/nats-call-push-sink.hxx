#pragma once

#include <feature/call/services/call-ports.hxx>

#include <json/value.h>

#include <memory>

class NatsBus;

namespace call_push_wire
{
Json::Value toJson(const CallPushMessage& message);
}

class NatsCallPushSink final : public CallPushSink
{
public:
  explicit NatsCallPushSink(std::shared_ptr<NatsBus> bus);

  [[nodiscard]] bool publish(const CallPushMessage& message) const override;

private:
  std::shared_ptr<NatsBus> bus_;
};
