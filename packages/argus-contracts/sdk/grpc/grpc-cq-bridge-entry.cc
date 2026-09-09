#include <grpc/impl/call.h>
#include <grpc-cq-bridge.hxx>

#include <memory>
#include <utility>

void grpc_call_run_cq_cb(const grpc_call* call,
                         absl::AnyInvocable<void()>&& callback)
{
  argus::bridgeCq(call, std::function<void()>(
      [held = std::make_shared<absl::AnyInvocable<void()>>(
           std::move(callback))] { (*held)(); }));
}

void grpc_call_run_in_event_engine(const grpc_call* call,
                                   absl::AnyInvocable<void()> callback)
{
  argus::bridgeEventEngine(call, std::function<void()>(
      [held = std::make_shared<absl::AnyInvocable<void()>>(
           std::move(callback))] { (*held)(); }));
}
