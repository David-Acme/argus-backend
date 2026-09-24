#include "route-announcements.hxx"

#include "logical-routes.hxx"

#include <routes/service-discovery.hxx>
#include <string>
#include <utility>

std::vector<MdnsInstance>
routeAnnouncementsFor(const std::vector<std::string>& routes,
                      const RouteAnnouncementInput& input)
{
  std::vector<MdnsInstance> instances;
  instances.reserve(routes.size());
  for (const std::string& route : routes) {
    std::vector<std::pair<std::string, std::string>> txt;
    txt.emplace_back(std::string(routes::kTxtPath), route);
    if (input.tls)
      txt.emplace_back(std::string(routes::kTxtHttps), "true");
    instances.push_back(
        MdnsInstance{.serviceType = std::string(routes::kServiceType),
                     .path = route,
                     .port = input.port,
                     .txt = std::move(txt)});
  }
  return instances;
}

std::vector<MdnsInstance>
routeAnnouncements(const RouteAnnouncementInput& input)
{
  return routeAnnouncementsFor(logicalRoutes(), input);
}
