#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/enrollment/repositories/enrollment/enrollment-repository.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/schemas/user/user-schema.hxx>
#include <optional>
#include <shared/services/face/face-check.hxx>
#include <shared/services/face/face-db.hxx>
#include <shared/services/face/member-match.hxx>
#include <shared/services/storage/private-portrait-service.hxx>
#include <string>
#include <vector>

struct EnrollmentInput
{
  std::string image;
  std::string name;
  std::string invitationToken;
  std::string lang;
  std::string deviceHash;
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
  FaceIndexFailed,
  AccountDisabled,
  LivenessFailed,
  LivenessUnavailable,
  FaceQualityInsufficient
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

  [[nodiscard]] static EnrollmentOutcome outcomeOf(FaceCheckStatus status);

private:
  struct Admission
  {
    std::optional<EnrollmentOutcome> refusal;
    bool initialOwner{false};
    UserRole role{UserRole::Owner};
    std::optional<UserInvitationSchema> invitation;
    std::string invitationHash;
  };

  [[nodiscard]] drogon::Task<Admission> admit(const EnrollmentInput& input) const;
  [[nodiscard]] drogon::Task<EnrollmentResult>
  signInRegistered(const MemberMatch& match) const;
  [[nodiscard]] static bool indexFace(const FaceInsertInput& input);

  MemberMatcher matcher_;
  EnrollmentRepository enrollmentRepository_;
  UserRepository userRepository_;
  PersonRepository personRepository_;
  UserInvitationRepository invitationRepository_;
  PrivatePortraitService privatePortraitService_;
};
