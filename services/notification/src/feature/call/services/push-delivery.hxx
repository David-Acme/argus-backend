#pragma once

#include <feature/call/services/call-ports.hxx>
#include <feature/call/services/push-sealer.hxx>
#include <feature/call/schemas/call/call-schema.hxx>

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

std::string callDeepLink(const std::string& callId);

class CallPushDelivery
{
public:
  CallPushDelivery(std::shared_ptr<const CallPushDeviceDirectory> devices,
                   std::shared_ptr<const CallPushSink> sink,
                   std::function<int64_t()> clock);

  drogon::Task<int64_t> ring(const CallSchema& call) const;
  drogon::Task<int64_t> cancel(const CallSchema& call) const;

private:
  drogon::Task<int64_t> deliver(const CallSchema& call, bool cancelling) const;

  std::shared_ptr<const CallPushDeviceDirectory> devices_;
  std::shared_ptr<const CallPushSink> sink_;
  std::function<int64_t()> clock_;
  PushSealer sealer_;
};
