#pragma once

#include <client/tunnel-client.hxx>
#include <controllers/health-controller.hxx>
#include <relay/tunnel-relay.hxx>

namespace tunnel
{

// The /health contract of each tunnel binary, sampled per request.
HealthStatus relayHealthStatus(TunnelRelay& relay);
HealthStatus clientHealthStatus(TunnelClient& client);

} // namespace tunnel
