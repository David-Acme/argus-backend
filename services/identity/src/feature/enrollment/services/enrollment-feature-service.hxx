#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/enrollment/repositories/enrollment/enrollment-repository.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/schemas/user/user-schema.hxx>
#include <shared/services/storage/private-portrait-service.hxx>
#include <string>
#include <vector>

struct EnrollmentInput
{
  std::string image;
  std::string name;
  std::string invitationToken;
  std::string lang;
};

enum class EnrollmentOutcome : uint8_t
{
  Enrolled,
  AlreadyRegistered,
  NotPaired,
  FaceExtractionFailed,
  FaceAlreadyRegistered,
  FaceNotRecognized,
  InvitationRequired,
  InvitationInvalid,
  OwnerAlreadyExists,
  FaceIndexFailed
};

struct EnrollmentResult
{
  EnrollmentOutcome outcome{EnrollmentOutcome::FaceExtractionFailed};
  int64_t userId{0};
  int64_t personId{0};
  std::string name;
  std::string lastName;
  std::string lang;
  UserRole role{UserRole::Guest};
};

class EnrollmentFeatureService
{
public:
  EnrollmentFeatureService() = default;
  ~EnrollmentFeatureService() = default;

  [[nodiscard]] drogon::Task<EnrollmentResult>
  registerUser(const EnrollmentInput& input) const;

private:
  [[nodiscard]] drogon::Task<EnrollmentResult>
  recognizeRegistered(const std::string& image) const;

  EnrollmentRepository enrollmentRepository_;
  UserRepository userRepository_;
  PersonRepository personRepository_;
  UserInvitationRepository invitationRepository_;
  PrivatePortraitService privatePortraitService_;
};
