#pragma once

#include <cstddef>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <memory>

struct SyncRegistrationStats
{
  size_t controllers;
  size_t filters;
};

// Registers the gateway /sync socket with the given forwarder (the legacy
// relay). The identity surface's DeviceFilter and JwtFilter serve the socket's
// filter chain and must already be registered.
SyncRegistrationStats registerSyncSurface(
    std::shared_ptr<SyncForwarder> forwarder);