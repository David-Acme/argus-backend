#include "face-service-adapter.hxx"

#include <shared/services/face/face-db.hxx>
#include <shared/services/face/face-service.hxx>

bool FaceServiceAdapter::initialize()
{
  FaceService::init();
  return true;
}

bool FaceServiceAdapter::isLoaded() const
{
  return FaceService::isLoaded();
}

void FaceServiceAdapter::shutdown()
{
  FaceService::shutdown();
}

Json::Value FaceServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = FaceService::isLoaded();
  value["embeddings"] = static_cast<Json::Int64>(FaceDB::count());
  return value;
}
