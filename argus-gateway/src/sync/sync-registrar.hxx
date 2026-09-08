#pragma once

#include <cstddef>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <memory>
#include <shared/contracts/camera-sync-source.hxx>

struct SyncRegistrationStats
{
  size_t controllers;
  size_t filters;
};

// Registers the gateway /sync socket with the given forwarder (the legacy
// relay) and the camera-domain pull source (argus-camera's gRPC leg). The
// identity surface's DeviceFilter and JwtFilter serve the socket's filter
// chain and must already be registered.
SyncRegistrationStats registerSyncSurface(
    std::shared_ptr<SyncForwarder> forwarder,
    std::shared_ptr<CameraSyncSource> cameraSource);
