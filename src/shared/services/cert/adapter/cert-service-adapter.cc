#include "cert-service-adapter.hxx"

#include <shared/services/cert/cert-service.hxx>

bool CertServiceAdapter::initialize()
{
  return CertService::init();
}

bool CertServiceAdapter::isLoaded() const
{
  return CertService::isLoaded();
}

void CertServiceAdapter::shutdown()
{
  CertService::shutdown();
}

Json::Value CertServiceAdapter::health() const
{
  return CertService::health();
}
