#pragma once

#include <config/service.hxx>
#include <shared/services/room/room-manager.hxx>

class RoomManagerServiceAdapter : public IService
{
public:
  std::string name() const override { return "room_manager"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

private:
  RoomManager roomManager_;
};
