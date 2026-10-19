#pragma once

#include <feature/proximity/services/proximity-service.hxx>

#include <json/value.h>
#include <vector>

struct ResponseProximityMeDto
{
  ProximityMeView me;

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponseProximityReceiversDto
{
  std::vector<ProximityReceiverView> receivers;

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponseProximityReceiverDto
{
  ProximityReceiverView receiver;

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponseProximitySensorsDto
{
  std::vector<ProximitySensorView> sensors;

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponseProximitySensorDto
{
  ProximitySensorView sensor;

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponseProximityPeopleDto
{
  std::vector<RoomVerdict> people;

  [[nodiscard]] Json::Value toJson() const;
};
