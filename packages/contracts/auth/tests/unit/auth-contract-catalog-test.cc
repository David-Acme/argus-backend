#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <auth/auth-errors.hxx>
#include <cstddef>
#include <doctest/doctest.h>
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
    {.name = "MissingToken",
     .definition = &AuthErrors::MissingToken,
     .code = ErrorCode::Unauthorized,
     .status = 401,
     .message = "Missing authorization token"},
    {.name = "AuthenticationRequired",
     .definition = &AuthErrors::AuthenticationRequired,
     .code = ErrorCode::Unauthorized,
     .status = 401,
     .message = "Authentication required"},
    {.name = "AccessDenied",
     .definition = &AuthErrors::AccessDenied,
     .code = ErrorCode::Forbidden,
     .status = 403,
     .message = "Access denied"},
    {.name = "InvalidJsonBody",
     .definition = &AuthErrors::InvalidJsonBody,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Invalid JSON body"},
    {.name = "InvalidMultipartForm",
     .definition = &AuthErrors::InvalidMultipartForm,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Invalid multipart form"},
    {.name = "MissingChallengeId",
     .definition = &AuthErrors::MissingChallengeId,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Missing challenge id"},
    {.name = "TooManyAttempts",
     .definition = &AuthErrors::TooManyAttempts,
     .code = ErrorCode::TooManyRequests,
     .status = 429,
     .message = "Too many requests"},
    {.name = "ChallengeNotFound",
     .definition = &AuthErrors::ChallengeNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Challenge not found"},
    {.name = "ChallengeExpired",
     .definition = &AuthErrors::ChallengeExpired,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Challenge expired"},
    {.name = "UserNotFound",
     .definition = &AuthErrors::UserNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "User not found"},
    {.name = "RefreshTokenInvalidOrExpired",
     .definition = &AuthErrors::RefreshTokenInvalidOrExpired,
     .code = ErrorCode::Unauthorized,
     .status = 401,
     .message = "Invalid or expired refresh token"},
    {.name = "LoginChallengeGenerationFailed",
     .definition = &AuthErrors::LoginChallengeGenerationFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 500,
     .message = "Failed to generate login challenge"},
    {.name = "DeviceCredentialIssuanceFailed",
     .definition = &AuthErrors::DeviceCredentialIssuanceFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 500,
     .message = "Failed to issue device credential"},
    {.name = "ChangeNotRecorded",
     .definition = &AuthErrors::ChangeNotRecorded,
     .code = ErrorCode::InternalError,
     .status = 500,
     .message = "The change could not be recorded"},
    {.name = "IdentityUnavailable",
     .definition = &AuthErrors::IdentityUnavailable,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "The identity service is unavailable"},
    {.name = "ServerNotPaired",
     .definition = &AuthErrors::ServerNotPaired,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "Server is not paired yet"},
    {.name = "FaceExtractionFailed",
     .definition = &AuthErrors::FaceExtractionFailed,
     .code = ErrorCode::BadRequest,
     .status = 422,
     .message = "Face could not be extracted"},
    {.name = "FaceNotRecognized",
     .definition = &AuthErrors::FaceNotRecognized,
     .code = ErrorCode::Unauthorized,
     .status = 401,
     .message = "Face not recognized"},
    {.name = "FaceAlreadyRegistered",
     .definition = &AuthErrors::FaceAlreadyRegistered,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "Face already registered"},
    {.name = "InvitationRequired",
     .definition = &AuthErrors::InvitationRequired,
     .code = ErrorCode::Forbidden,
     .status = 403,
     .message = "A valid invitation is required"},
    {.name = "InvitationInvalidOrExpired",
     .definition = &AuthErrors::InvitationInvalidOrExpired,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Invitation is invalid or expired"},
    {.name = "OwnerAlreadyExists",
     .definition = &AuthErrors::OwnerAlreadyExists,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "An owner already exists"},
    {.name = "EnrolledFaceIndexFailed",
     .definition = &AuthErrors::EnrolledFaceIndexFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Could not index enrolled face"},
};

constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the auth catalog matches the table pinned here")
{
  for (const auto& entry : kCatalog) {
    CAPTURE(entry.name);
    CHECK(std::string(entry.definition->wireCode()) ==
          std::string(toString(entry.code)));
    CHECK(entry.definition->status == entry.status);
    CHECK(std::string(entry.definition->message) == entry.message);
  }
  CHECK(kCatalog.size() == 23);
}

TEST_CASE("every auth entry is legal on the wire")
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

TEST_CASE("no two auth entries say the same thing")
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
