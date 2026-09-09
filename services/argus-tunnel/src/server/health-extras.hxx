#pragma once

#include <client/tunnel-client.hxx>
#include <controllers/health-controller.hxx>
#include <relay/tunnel-relay.hxx>

namespace tunnel
{

// The /health contract of each tunnel binary: the field set the fleet reads,
// built as providers so every field is sampled per request rather than at
// boot. The relay adds pushForwarded, which the client has no counterpart for.
HealthStatus relayHealthStatus(TunnelRelay& relay);
HealthStatus clientHealthStatus(TunnelClient& client);

} // namespace tunnel
