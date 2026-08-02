#include "room-manager-service-adapter.hxx"

bool RoomManagerServiceAdapter::initialize()
{
  roomManager_.init();
  return true;
}

bool RoomManagerServiceAdapter::isLoaded() const
{
  return true;
}

void RoomManagerServiceAdapter::shutdown()
{
  roomManager_.shutdown();
}

Json::Value RoomManagerServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = true;
  return value;
}
