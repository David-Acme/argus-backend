#include "face-sign-in-service.hxx"

#include <config/identity-config.hxx>
#include <shared/services/face/face-service.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<FaceSignInResult> FaceSignInService::identify(std::string image) const
{
  const IdentityFaceConfig config = IdentityConfig::resolveFace();
  auto check = co_await FaceService::instance().verifyImageAsync(
      {.imageBytes = std::move(image),
       .policy = {.quality = face_check::biometricGate(),
                  .livenessRequired = config.livenessRequired,
                  .livenessThreshold = config.livenessThreshold,
                  .encodePortrait = false}});
  FaceSignInResult result{.check = check.status, .match = std::nullopt};
  if (check.status != FaceCheckStatus::Accepted) {
    LOG_DEBUG << "Face sign-in refused: " << std::string(face_check::statusToString(check.status));
    co_return result;
  }
  result.match = co_await matcher_.match({.embedding = std::move(check.embedding),
                                          .threshold = FaceDB::matchThreshold(),
                                          .margin = config.loginMargin});
  LOG_DEBUG << "Face sign-in " << (result.match ? "matched" : "found no member");
  co_return result;
}
