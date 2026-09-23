#pragma once

#include <client/tunnel-client.hxx>
#include <http/health-controller.hxx>
#include <relay/tunnel-relay.hxx>

namespace tunnel
{

HealthStatus relayHealthStatus(TunnelRelay& relay);
HealthStatus clientHealthStatus(TunnelClient& client);

}
