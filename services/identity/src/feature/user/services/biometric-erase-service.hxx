#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/user/dtos/response-biometric-erase-dto.hxx>
#include <feature/user/repositories/biometric-erase/biometric-erase-repository.hxx>
#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <shared/repositories/pending-object-delete/pending-object-delete-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>

struct BiometricEraseRequest
{
  int64_t actorId{0};
  UserRole actorRole{UserRole::Unknown};
  int64_t subjectId{0};
};

class BiometricEraseService
{
public:
  [[nodiscard]] drogon::Task<ResponseBiometricEraseDto>
  erase(const BiometricEraseRequest& request) const;

private:
  BiometricEraseRepository repository_;
  UserRepository userRepository_;
  PendingObjectDeleteRepository pendingRepository_;
  VoiceprintFeatureService voiceprint_;
};
