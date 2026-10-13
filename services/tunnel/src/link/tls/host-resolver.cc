#include <link/tls/host-resolver.hxx>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace tunnel::link
{
std::vector<Endpoint> resolveHost(std::string_view host, uint16_t port)
{
  std::vector<Endpoint> endpoints;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  const std::string name{host};
  const std::string service = std::to_string(port);
  if (::getaddrinfo(name.c_str(), service.c_str(), &hints, &found) != 0)
    return endpoints;
  for (addrinfo* entry = found; entry != nullptr; entry = entry->ai_next) {
    if (entry->ai_addr == nullptr ||
        entry->ai_addrlen < sizeof(sockaddr_in))
      continue;
    const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
    std::array<char, INET_ADDRSTRLEN> text{};
    if (::inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size()) ==
        nullptr)
      continue;
    endpoints.push_back({.ip = text.data(), .port = port});
    if (endpoints.size() >= 4)
      break;
  }
  ::freeaddrinfo(found);
  return endpoints;
}
}
