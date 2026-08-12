#include "face-service-adapter.hxx"

#include <shared/services/face/face-db.hxx>
#include <shared/services/face/face-service.hxx>

bool FaceServiceAdapter::initialize()
{
  FaceService::instance().init();
  return true;
}

bool FaceServiceAdapter::isLoaded() const
{
  return FaceService::instance().isLoaded();
}

void FaceServiceAdapter::shutdown()
{
  FaceService::instance().shutdown();
}

Json::Value FaceServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = FaceService::instance().isLoaded();
  value["embeddings"] =
      static_cast<Json::Int64>(FaceService::instance().faceDb().count());
  return value;
}
