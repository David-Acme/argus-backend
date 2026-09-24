#pragma once

#include <cstdint>
#include <mdns/mdns-service.hxx>
#include <string>
#include <vector>

struct RouteAnnouncementInput
{
  uint16_t port{0};
  bool tls{false};
};

std::vector<MdnsInstance>
routeAnnouncements(const RouteAnnouncementInput& input);
std::vector<MdnsInstance>
routeAnnouncementsFor(const std::vector<std::string>& routes,
                      const RouteAnnouncementInput& input);
