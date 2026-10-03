#pragma once

#include <http/listener-config.hxx>
#include <string>

namespace certificate_reload
{

struct CertificatePair
{
  std::string certPath;
  std::string keyPath;
};

[[nodiscard]] bool usablePair(const CertificatePair& pair);

void watch(const ListenerConfig& listener);

}
