#include "grpc-cq-bridge.hxx"

#include <grpc/impl/call.h>
#include <utility>

namespace argus
{

void bridgeCq(const grpc_call* call, std::function<void()>&& callback)
{
  grpc_call_run_cq_cb(call, [callback = std::move(callback)] { callback(); });
}

void bridgeEventEngine(const grpc_call* call, std::function<void()>&& callback)
{
  grpc_call_run_in_event_engine(call,
                                [callback = std::move(callback)] { callback(); });
}

} // namespace argus
