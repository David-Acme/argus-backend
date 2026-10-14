#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace tunnel::link
{
enum class HomeService : uint8_t
{
  Auth,
  Identity,
  Sync,
  Camera,
  Productivity,
  Notification,
  Guard,
  Settings,
  Rtc,
  Turn,
};

inline constexpr std::array<HomeService, 10> kHomeServices{
    HomeService::Auth,         HomeService::Identity, HomeService::Sync,
    HomeService::Camera,       HomeService::Productivity,
    HomeService::Notification, HomeService::Guard,    HomeService::Settings,
    HomeService::Rtc,          HomeService::Turn};

std::string_view homeServiceToString(HomeService service) noexcept;
std::optional<HomeService> homeServiceFromString(std::string_view name) noexcept;
}
