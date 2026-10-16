#pragma once

#include <feature/call/schemas/call/call-schema.hxx>
#include <shared/services/push-delivery/push-delivery-outbox.hxx>
#include <shared/services/push-delivery/push-device.hxx>
#include <shared/services/push-delivery/push-sealer.hxx>

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

std::string callDeepLink(const std::string& callId);

struct CallPushDeliveryDependencies
{
  std::shared_ptr<const PushDeviceDirectory> devices;
  std::shared_ptr<const PushDeliveryOutbox> outbox;
  std::function<int64_t()> clock;
};

struct CallPushInput
{
  CallSchema call;
  bool cancelling{false};
  drogon::orm::DbClient* client{nullptr};
  std::string exceptSessionId;
};

struct CallClaimCancelInput
{
  CallSchema call;
  drogon::orm::DbClient* client{nullptr};
  std::string claimedBySessionId;
};

class CallPushDelivery
{
public:
  explicit CallPushDelivery(CallPushDeliveryDependencies dependencies);

  [[nodiscard]] drogon::Task<int64_t> ring(const CallSchema& call,
                                           drogon::orm::DbClient* client) const;
  [[nodiscard]] drogon::Task<int64_t> cancel(const CallSchema& call,
                                             drogon::orm::DbClient* client) const;
  [[nodiscard]] drogon::Task<int64_t> cancelOthers(const CallClaimCancelInput& input) const;

private:
  [[nodiscard]] drogon::Task<int64_t> deliver(CallPushInput input) const;

  std::shared_ptr<const PushDeviceDirectory> devices_;
  std::shared_ptr<const PushDeliveryOutbox> outbox_;
  std::function<int64_t()> clock_;
  PushSealer sealer_;
};
