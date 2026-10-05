#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <array>
#include <cstddef>
#include <doctest/doctest.h>
#include <identity/identity-errors.hxx>
#include <string>

struct CatalogEntry
{
  const char* name;
  const ErrorDefinition* definition;
  ErrorCode code;
  int status;
  const char* message;
};

constexpr std::array<CatalogEntry, 38> kCatalog{{
    {.name = "FaceNotRecognized",
     .definition = &IdentityErrors::FaceNotRecognized,
     .code = ErrorCode::Unauthorized,
     .status = 401,
     .message = "Face not recognized"},
    {.name = "ServerNotPaired",
     .definition = &IdentityErrors::ServerNotPaired,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "Server is not paired yet"},
    {.name = "FaceExtractionFailed",
     .definition = &IdentityErrors::FaceExtractionFailed,
     .code = ErrorCode::BadRequest,
     .status = 422,
     .message = "Face could not be extracted"},
    {.name = "FaceAlreadyRegistered",
     .definition = &IdentityErrors::FaceAlreadyRegistered,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "Face already registered"},
    {.name = "InvitationRequired",
     .definition = &IdentityErrors::InvitationRequired,
     .code = ErrorCode::Forbidden,
     .status = 403,
     .message = "A valid invitation is required"},
    {.name = "InvitationInvalidOrExpired",
     .definition = &IdentityErrors::InvitationInvalidOrExpired,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Invitation is invalid or expired"},
    {.name = "OwnerAlreadyExists",
     .definition = &IdentityErrors::OwnerAlreadyExists,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "An owner already exists"},
    {.name = "EnrolledFaceIndexFailed",
     .definition = &IdentityErrors::EnrolledFaceIndexFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Could not index enrolled face"},
    {.name = "LoginChallengeGenerationFailed",
     .definition = &IdentityErrors::LoginChallengeGenerationFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 500,
     .message = "Failed to generate login challenge"},
    {.name = "ChallengeNotFound",
     .definition = &IdentityErrors::ChallengeNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Challenge not found"},
    {.name = "ChallengeExpired",
     .definition = &IdentityErrors::ChallengeExpired,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Challenge expired"},
    {.name = "RefreshTokenInvalidOrExpired",
     .definition = &IdentityErrors::RefreshTokenInvalidOrExpired,
     .code = ErrorCode::Unauthorized,
     .status = 401,
     .message = "Invalid or expired refresh token"},
    {.name = "UserNotFound",
     .definition = &IdentityErrors::UserNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "User not found"},
    {.name = "DeviceCredentialIssuanceFailed",
     .definition = &IdentityErrors::DeviceCredentialIssuanceFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 500,
     .message = "Failed to issue device credential"},
    {.name = "InvitationCreationFailed",
     .definition = &IdentityErrors::InvitationCreationFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Unable to create invitation"},
    {.name = "InvitationResolutionFailed",
     .definition = &IdentityErrors::InvitationResolutionFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Unable to resolve invitation"},
    {.name = "InvitationOwnerAccessForbidden",
     .definition = &IdentityErrors::InvitationOwnerAccessForbidden,
     .code = ErrorCode::BadRequest,
     .status = 422,
     .message = "Invitations cannot grant owner access"},
    {.name = "InvitationNotFound",
     .definition = &IdentityErrors::InvitationNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Invitation not found"},
    {.name = "ServerCertificateUnavailable",
     .definition = &IdentityErrors::ServerCertificateUnavailable,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Server certificate is unavailable"},
    {.name = "PortraitPreviewPreparationFailed",
     .definition = &IdentityErrors::PortraitPreviewPreparationFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Unable to prepare portrait preview"},
    {.name = "PortraitVerificationUnavailable",
     .definition = &IdentityErrors::PortraitVerificationUnavailable,
     .code = ErrorCode::Forbidden,
     .status = 403,
     .message = "Portrait verification is not available"},
    {.name = "InvalidUserId",
     .definition = &IdentityErrors::InvalidUserId,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Invalid user id"},
    {.name = "PortraitUnavailable",
     .definition = &IdentityErrors::PortraitUnavailable,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Portrait is unavailable"},
    {.name = "PortraitPreviewUnavailable",
     .definition = &IdentityErrors::PortraitPreviewUnavailable,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Portrait preview is unavailable"},
    {.name = "ActiveOwnerRequired",
     .definition = &IdentityErrors::ActiveOwnerRequired,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "At least one active owner is required"},
    {.name = "SelfDeactivationForbidden",
     .definition = &IdentityErrors::SelfDeactivationForbidden,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "You cannot deactivate your own account"},
    {.name = "InvalidMultipartForm",
     .definition = &IdentityErrors::InvalidMultipartForm,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Invalid multipart form"},
    {.name = "MissingChallengeId",
     .definition = &IdentityErrors::MissingChallengeId,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Missing challenge id"},
    {.name = "InvalidInvitationId",
     .definition = &IdentityErrors::InvalidInvitationId,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Invalid invitation id"},
    {.name = "InvalidPairingCode",
     .definition = &IdentityErrors::InvalidPairingCode,
     .code = ErrorCode::Forbidden,
     .status = 403,
     .message = "Invalid pairing code"},
    {.name = "ChangeNotRecorded",
     .definition = &IdentityErrors::ChangeNotRecorded,
     .code = ErrorCode::InternalError,
     .status = 500,
     .message = "The change could not be recorded"},
    {.name = "VoiceprintNotFound",
     .definition = &IdentityErrors::VoiceprintNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Argus has not learned this person's voice"},
    {.name = "VisitorNotFound",
     .definition = &IdentityErrors::VisitorNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "This person is not in the gallery"},
    {.name = "VisitorSampleNotFound",
     .definition = &IdentityErrors::VisitorSampleNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "This face sample is not part of the person"},
    {.name = "VisitorMergeInvalid",
     .definition = &IdentityErrors::VisitorMergeInvalid,
     .code = ErrorCode::BadRequest,
     .status = 422,
     .message = "Choose other people to merge into this one"},
    {.name = "VisitorSplitInvalid",
     .definition = &IdentityErrors::VisitorSplitInvalid,
     .code = ErrorCode::BadRequest,
     .status = 422,
     .message = "Choose some, but not all, of the person's face samples"},
    {.name = "VisitorCropUnavailable",
     .definition = &IdentityErrors::VisitorCropUnavailable,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "The face picture is unavailable"},
    {.name = "VisitorRecognitionOff",
     .definition = &IdentityErrors::VisitorRecognitionOff,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "Recognition of recurring visitors is turned off"},
}};

constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the identity catalog matches the table pinned here")
{
  for (const auto& entry : kCatalog) {
    CAPTURE(entry.name);
    CHECK(std::string(entry.definition->wireCode()) ==
          std::string(toString(entry.code)));
    CHECK(entry.definition->status == entry.status);
    CHECK(std::string(entry.definition->message) == entry.message);
  }
  CHECK(kCatalog.size() == 38);
}

TEST_CASE("every identity entry is legal on the wire")
{
  for (const auto& entry : kCatalog) {
    CAPTURE(entry.name);
    const std::string message(entry.definition->message);
    const std::string wire(entry.definition->wireCode());
    CHECK(entry.definition->status >= 400);
    CHECK(entry.definition->status <= 599);
    CHECK(!message.empty());
    CHECK(message.size() <= kMaxMessageBytes);
    CHECK(message.find('\0') == std::string::npos);
    CHECK(!wire.empty());
    CHECK(wire.size() <= kMaxCodeBytes);
    CHECK(std::ranges::all_of(wire, [](unsigned char byte) {
      return byte >= 0x21 && byte <= 0x7e;
    }));
  }
}

TEST_CASE("no two identity entries say the same thing")
{
  for (std::size_t i = 0; i < kCatalog.size(); ++i) {
    for (std::size_t j = i + 1; j < kCatalog.size(); ++j) {
      CAPTURE(i);
      CAPTURE(j);
      CHECK(std::string(kCatalog[i].definition->message) !=
            kCatalog[j].definition->message);
    }
  }
}

TEST_CASE(
    "the identity entries that contradict their own code are the known ones")
{
  std::vector<std::string> offenders;
  for (const auto& entry : kCatalog) {
    if (entry.definition->wireCode() != "SERVICE_UNAVAILABLE")
      continue;
    if (entry.definition->status != 503)
      offenders.push_back(entry.name);
  }
  const std::vector<std::string> known{"LoginChallengeGenerationFailed",
                                       "DeviceCredentialIssuanceFailed"};
  CHECK(offenders == known);
}
