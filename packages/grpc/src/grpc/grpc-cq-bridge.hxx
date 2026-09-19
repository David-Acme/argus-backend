#pragma once

#include <functional>
#include <grpc/grpc.h>

namespace argus
{

void bridgeCq(const grpc_call* call, std::function<void()>&& callback);
void bridgeEventEngine(const grpc_call* call, std::function<void()>&& callback);

} // namespace argus
