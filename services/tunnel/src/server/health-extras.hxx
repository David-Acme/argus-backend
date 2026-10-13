#pragma once

#include <client/tunnel-client.hxx>
#include <http/health-controller.hxx>
#include <link/client/link-client.hxx>
#include <relay/tunnel-relay.hxx>

namespace tunnel
{

HealthStatus relayHealthStatus(TunnelRelay& relay);
HealthStatus clientHealthStatus(TunnelClient& client);
HealthStatus linkHealthStatus(link::LinkClient& client);

}
