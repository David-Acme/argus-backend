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

// Registers the /sync socket with the legacy relay and the camera gRPC leg.
SyncRegistrationStats registerSyncSurface(
    std::shared_ptr<SyncForwarder> forwarder,
    std::shared_ptr<CameraSyncSource> cameraSource);
