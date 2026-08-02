#include "mdns-service-adapter.hxx"

#include <drogon/drogon.h>

bool MdnsServiceAdapter::initialize()
{
  service_ = std::make_unique<MdnsService>();
  const bool ok = service_->initialize();
  if (ok && !service_->isAdvertising())
    LOG_WARN << "mDNS adapter: service is not advertising";
  return ok;
}

bool MdnsServiceAdapter::isLoaded() const
{
  return service_ != nullptr;
}

void MdnsServiceAdapter::shutdown()
{
  if (service_)
    service_->shutdown();
  service_.reset();
}

Json::Value MdnsServiceAdapter::health() const
{
  return service_ ? service_->health() : Json::Value(Json::objectValue);
}
