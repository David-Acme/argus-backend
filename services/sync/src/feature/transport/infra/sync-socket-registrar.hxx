#pragma once

#include <cstddef>
#include <sync/sync-forwarder.hxx>
#include <memory>
#include <feature/transport/infra/camera-sync-source.hxx>
#include <feature/transport/infra/notification-sync-source.hxx>
#include <feature/transport/infra/productivity-sync-source.hxx>

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

SyncRegistrationStats registerSyncSurface(SyncSurfaceInput input);
