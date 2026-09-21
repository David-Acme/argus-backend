#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <cstddef>
#include <doctest/doctest.h>
#include <identity/identity-errors.hxx>
#include <string>
#include <vector>

// The identity boundary, pinned as a table. Naming every entry here is the
// point: an edited status, or a message that quietly loses a word, has to be
// edited twice -- here and in the header -- so the diff shows it.
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

// What response-rpc.cc's validRecord accepts before a refusal can be
// serialized: an entry outside these limits can never reach a client, the
// serializer would answer 502 for it instead.
constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the identity catalog matches the table pinned here")
{
  for (const auto& entry : kCatalog) {
    CAPTURE(entry.name);
    // The code column is compared as its wire string, which is the value
    // the errors package pins one-to-one for every enumerator: the same
    // assertion, and a failure that prints NOT_FOUND instead of a number.
    CHECK(std::string(entry.definition->wireCode()) ==
          std::string(toString(entry.code)));
    CHECK(entry.definition->status == entry.status);
    CHECK(std::string(entry.definition->message) == entry.message);
  }
  // A new entry in the header compiles and fails nothing above, because
  // nothing above knows the catalog grew. This is the tripwire: the count
  // only holds once the entry is in the table too.
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
    // The serializer's rule for a message, mirrored exactly: non-empty, at
    // most 1024 bytes, no NUL. Deliberately not restricted to ASCII -- a
    // UTF-8 message is legal on the wire, and a suite that forbade one would
    // block a legitimate translation.
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
  // Five catalogs in the tree carry a SERVICE_UNAVAILABLE entry and four of
  // them answer 503. These two answer 500. Measured, recorded, and left alone
  // -- both call sites pass 500 explicitly, so the catalog is not what a
  // client sees
  // (packages/identity/src/feature/api/auth/services/auth-service.cc:330 and
  // :574). Flagged in the step's report rather than fixed in a layout step;
  // the list fails the suite the day a third one arrives.
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
