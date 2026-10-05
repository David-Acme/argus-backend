#pragma once

#include <cstddef>
#include <sync/sync-forwarder.hxx>
#include <memory>
#include <auth/user-directory.hxx>
#include <feature/transport/infra/camera-sync-source.hxx>
#include <feature/transport/infra/heartbeat-source.hxx>
#include <feature/transport/infra/identity-sync-source.hxx>
#include <feature/transport/infra/notification-sync-source.hxx>
#include <feature/transport/infra/productivity-sync-source.hxx>
#include <feature/transport/services/connection-lanes.hxx>

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
  std::shared_ptr<IdentitySyncSource> identitySource;
  std::shared_ptr<const IUserDirectory> userDirectory;
  std::shared_ptr<const HeartbeatSource> heartbeatSource;
  std::shared_ptr<ConnectionLanes> lanes;
};

SyncRegistrationStats registerSyncSurface(SyncSurfaceInput input);
