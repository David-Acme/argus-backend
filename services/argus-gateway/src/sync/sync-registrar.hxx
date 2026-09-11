#pragma once

#include <cstddef>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <memory>
#include <shared/contracts/camera-sync-source.hxx>
#include <shared/contracts/notification-sync-source.hxx>
#include <shared/contracts/productivity-sync-source.hxx>

struct SyncRegistrationStats
{
  size_t controllers;
  size_t filters;
};

struct SyncSurfaceInput
{
  std::shared_ptr<SyncForwarder> forwarder;
  std::shared_ptr<CameraSyncSource> cameraSource;
  std::shared_ptr<ProductivitySyncSource> productivitySource;
  std::shared_ptr<NotificationSyncSource> notificationSource;
};

// Registers the /sync socket with the legacy relay and the domain gRPC legs.
SyncRegistrationStats registerSyncSurface(SyncSurfaceInput input);
