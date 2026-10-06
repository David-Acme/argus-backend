#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace AuthErrors
{
inline constexpr ErrorDefinition MissingToken{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Missing authorization token"};
inline constexpr ErrorDefinition AuthenticationRequired{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Authentication required"};
inline constexpr ErrorDefinition AccessDenied{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "Access denied"};
inline constexpr ErrorDefinition RemoteNotAllowed{
    .code = ErrorCode::RemoteNotAllowed,
    .status = 403,
    .message = "Remote requests are not allowed"};
inline constexpr ErrorDefinition ModuleDisabled{
    .code = ErrorCode::ModuleDisabled,
    .status = 403,
    .message = "This module is disabled"};
inline constexpr ErrorDefinition RoleInactive{
    .code = ErrorCode::RoleInactive,
    .status = 403,
    .message = "This role is inactive while its module is off"};
inline constexpr ErrorDefinition InvalidJsonBody{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid JSON body"};
inline constexpr ErrorDefinition InvalidMultipartForm{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid multipart form"};
inline constexpr ErrorDefinition MissingChallengeId{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Missing challenge id"};
inline constexpr ErrorDefinition TooManyAttempts{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Too many requests"};
inline constexpr ErrorDefinition ChallengeNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Challenge not found"};
inline constexpr ErrorDefinition ChallengeExpired{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Challenge expired"};
inline constexpr ErrorDefinition UserNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "User not found"};
inline constexpr ErrorDefinition RefreshTokenInvalidOrExpired{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Invalid or expired refresh token"};
inline constexpr ErrorDefinition LoginChallengeGenerationFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 500,
    .message = "Failed to generate login challenge"};
inline constexpr ErrorDefinition DeviceCredentialIssuanceFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 500,
    .message = "Failed to issue device credential"};
inline constexpr ErrorDefinition TokenIssuanceFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 500,
    .message = "Failed to issue token"};
inline constexpr ErrorDefinition ChangeNotRecorded{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The change could not be recorded"};
inline constexpr ErrorDefinition AuthUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "The auth service is unavailable"};
inline constexpr ErrorDefinition AuthBusy{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "The server is busy, retry shortly"};
inline constexpr ErrorDefinition IdentityUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "The identity service is unavailable"};
inline constexpr ErrorDefinition ServerNotPaired{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "Server is not paired yet"};
inline constexpr ErrorDefinition FaceExtractionFailed{
    .code = ErrorCode::BadRequest,
    .status = 422,
    .message = "Face could not be extracted"};
inline constexpr ErrorDefinition FaceNotRecognized{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Face not recognized"};
inline constexpr ErrorDefinition FaceAlreadyRegistered{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "Face already registered"};
inline constexpr ErrorDefinition InvitationRequired{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "A valid invitation is required"};
inline constexpr ErrorDefinition InvitationInvalidOrExpired{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Invitation is invalid or expired"};
inline constexpr ErrorDefinition OwnerAlreadyExists{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "An owner already exists"};
inline constexpr ErrorDefinition EnrolledFaceIndexFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Could not index enrolled face"};
inline constexpr ErrorDefinition SessionNotFound{
    .code = ErrorCode::SessionNotFound,
    .status = 404,
    .message = "Session not found"};
inline constexpr ErrorDefinition AccountDisabled{
    .code = ErrorCode::AccountDisabled,
    .status = 403,
    .message = "User account is disabled"};
inline constexpr ErrorDefinition LivenessCheckFailed{
    .code = ErrorCode::LivenessCheckFailed,
    .status = 401,
    .message = "The face did not pass the liveness check"};
inline constexpr ErrorDefinition LivenessUnavailable{
    .code = ErrorCode::LivenessUnavailable,
    .status = 503,
    .message = "The liveness check is unavailable"};
inline constexpr ErrorDefinition FaceQualityInsufficient{
    .code = ErrorCode::FaceQualityInsufficient,
    .status = 422,
    .message = "One clear, well-lit face is required"};
inline constexpr ErrorDefinition DeviceContextMissing{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The route binds no device to the session"};
}
