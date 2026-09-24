#pragma once

#include <cstdint>
#include <string>

struct UserContext
{
  int64_t userId{0};
  std::string name;
  std::string lastName;
  std::string lang;
  std::string role;
  bool isActive{false};
};
