#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tunnel::link
{
struct Endpoint
{
  std::string ip;
  uint16_t port{0};
};

std::vector<Endpoint> resolveHost(std::string_view host, uint16_t port);
}
