#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class GuardEpisodeKind : uint8_t
{
  Person,
  Camera
};

struct GuardEpisodeStatus
{
  GuardEpisodeKind kind{GuardEpisodeKind::Person};
  std::string subject{};
  std::string danger{};
  std::string camera{};
  int64_t at{0};
  bool resolved{false};
  std::string resolution{};
};

struct GuardEnvironmentStatus
{
  int64_t id{0};
  std::string name{};
  std::string mode{};
  bool armed{false};
  std::optional<GuardEpisodeStatus> latest{};
};

using GuardStatusDirectory = std::function<drogon::Task<std::vector<GuardEnvironmentStatus>>()>;
