#include <link/client/home-service.hxx>

#include <algorithm>

namespace tunnel::link
{
std::string_view homeServiceToString(HomeService service) noexcept
{
  switch (service) {
  case HomeService::Auth:
    return "auth";
  case HomeService::Identity:
    return "identity";
  case HomeService::Sync:
    return "sync";
  case HomeService::Camera:
    return "camera";
  case HomeService::Productivity:
    return "productivity";
  case HomeService::Notification:
    return "notification";
  case HomeService::Guard:
    return "guard";
  case HomeService::Settings:
    return "settings";
  }
  return "unknown";
}

std::optional<HomeService> homeServiceFromString(std::string_view name) noexcept
{
  const auto found = std::ranges::find_if(kHomeServices, [name](HomeService service) {
    return homeServiceToString(service) == name;
  });
  if (found == kHomeServices.end())
    return std::nullopt;
  return *found;
}
}
