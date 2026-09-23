#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <cstddef>
#include <doctest/doctest.h>
#include <identity/identity-errors.hxx>
#include <string>
#include <vector>

struct CatalogEntry
{
  const char* name;
  const ErrorDefinition* definition;
  ErrorCode code;
  int status;
  const char* message;
};

const std::vector<CatalogEntry> kCatalog{
    {"FaceNotRecognized", &IdentityErrors::FaceNotRecognized,
     ErrorCode::Unauthorized, 401, "Face not recognized"},
    {"ServerNotPaired", &IdentityErrors::ServerNotPaired, ErrorCode::Conflict,
     409, "Server is not paired yet"},
    {"FaceExtractionFailed", &IdentityErrors::FaceExtractionFailed,
     ErrorCode::BadRequest, 422, "Face could not be extracted"},
    {"FaceAlreadyRegistered", &IdentityErrors::FaceAlreadyRegistered,
     ErrorCode::Conflict, 409, "Face already registered"},
    {"InvitationRequired", &IdentityErrors::InvitationRequired,
     ErrorCode::Forbidden, 403, "A valid invitation is required"},
    {"InvitationInvalidOrExpired", &IdentityErrors::InvitationInvalidOrExpired,
     ErrorCode::NotFound, 404, "Invitation is invalid or expired"},
    {"OwnerAlreadyExists", &IdentityErrors::OwnerAlreadyExists,
     ErrorCode::Conflict, 409, "An owner already exists"},
    {"EnrolledFaceIndexFailed", &IdentityErrors::EnrolledFaceIndexFailed,
     ErrorCode::ServiceUnavailable, 503, "Could not index enrolled face"},
    {"LoginChallengeGenerationFailed",
     &IdentityErrors::LoginChallengeGenerationFailed,
     ErrorCode::ServiceUnavailable, 500, "Failed to generate login challenge"},
    {"ChallengeNotFound", &IdentityErrors::ChallengeNotFound,
     ErrorCode::NotFound, 404, "Challenge not found"},
    {"ChallengeExpired", &IdentityErrors::ChallengeExpired, ErrorCode::NotFound,
     404, "Challenge expired"},
    {"RefreshTokenInvalidOrExpired",
     &IdentityErrors::RefreshTokenInvalidOrExpired, ErrorCode::Unauthorized,
     401, "Invalid or expired refresh token"},
    {"UserNotFound", &IdentityErrors::UserNotFound, ErrorCode::NotFound, 404,
     "User not found"},
    {"DeviceCredentialIssuanceFailed",
     &IdentityErrors::DeviceCredentialIssuanceFailed,
     ErrorCode::ServiceUnavailable, 500, "Failed to issue device credential"},
    {"InvitationCreationFailed", &IdentityErrors::InvitationCreationFailed,
     ErrorCode::ServiceUnavailable, 503, "Unable to create invitation"},
    {"InvitationResolutionFailed", &IdentityErrors::InvitationResolutionFailed,
     ErrorCode::ServiceUnavailable, 503, "Unable to resolve invitation"},
    {"InvitationOwnerAccessForbidden",
     &IdentityErrors::InvitationOwnerAccessForbidden, ErrorCode::BadRequest,
     422, "Invitations cannot grant owner access"},
    {"InvitationNotFound", &IdentityErrors::InvitationNotFound,
     ErrorCode::NotFound, 404, "Invitation not found"},
    {"ServerCertificateUnavailable",
     &IdentityErrors::ServerCertificateUnavailable,
     ErrorCode::ServiceUnavailable, 503, "Server certificate is unavailable"},
    {"PortraitPreviewPreparationFailed",
     &IdentityErrors::PortraitPreviewPreparationFailed,
     ErrorCode::ServiceUnavailable, 503, "Unable to prepare portrait preview"},
    {"PortraitVerificationUnavailable",
     &IdentityErrors::PortraitVerificationUnavailable, ErrorCode::Forbidden,
     403, "Portrait verification is not available"},
    {"InvalidUserId", &IdentityErrors::InvalidUserId, ErrorCode::BadRequest,
     400, "Invalid user id"},
    {"PortraitUnavailable", &IdentityErrors::PortraitUnavailable,
     ErrorCode::NotFound, 404, "Portrait is unavailable"},
    {"PortraitPreviewUnavailable", &IdentityErrors::PortraitPreviewUnavailable,
     ErrorCode::NotFound, 404, "Portrait preview is unavailable"},
    {"ActiveOwnerRequired", &IdentityErrors::ActiveOwnerRequired,
     ErrorCode::Conflict, 409, "At least one active owner is required"},
    {"InvalidMultipartForm", &IdentityErrors::InvalidMultipartForm,
     ErrorCode::BadRequest, 400, "Invalid multipart form"},
    {"MissingChallengeId", &IdentityErrors::MissingChallengeId,
     ErrorCode::BadRequest, 400, "Missing challenge id"},
    {"InvalidInvitationId", &IdentityErrors::InvalidInvitationId,
     ErrorCode::BadRequest, 400, "Invalid invitation id"},
    {"ServerAlreadyPaired", &IdentityErrors::ServerAlreadyPaired,
     ErrorCode::Conflict, 409, "Server already paired"},
    {"InvalidPairingCode", &IdentityErrors::InvalidPairingCode,
     ErrorCode::Forbidden, 403, "Invalid pairing code"},
};

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
  CHECK(kCatalog.size() == 30);
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
