#pragma once

#include <error-definition.hxx>

namespace IdentityErrors
{
inline constexpr ErrorDefinition FaceNotRecognized{
    .code = ErrorCode::Unauthorized,
    .message = "Face not recognized"};
inline constexpr ErrorDefinition ServerNotPaired{
    .code = ErrorCode::Conflict,
    .message = "Server is not paired yet"};
inline constexpr ErrorDefinition FaceExtractionFailed{
    .code = ErrorCode::BadRequest,
    .message = "Face could not be extracted"};
inline constexpr ErrorDefinition FaceAlreadyRegistered{
    .code = ErrorCode::Conflict,
    .message = "Face already registered"};
inline constexpr ErrorDefinition InvitationRequired{
    .code = ErrorCode::Forbidden,
    .message = "A valid invitation is required"};
inline constexpr ErrorDefinition InvitationInvalidOrExpired{
    .code = ErrorCode::NotFound,
    .message = "Invitation is invalid or expired"};
inline constexpr ErrorDefinition OwnerAlreadyExists{
    .code = ErrorCode::Conflict,
    .message = "An owner already exists"};
inline constexpr ErrorDefinition EnrolledFaceIndexFailed{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Could not index enrolled face"};
inline constexpr ErrorDefinition LoginChallengeGenerationFailed{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Failed to generate login challenge"};
inline constexpr ErrorDefinition ChallengeNotFound{
    .code = ErrorCode::NotFound,
    .message = "Challenge not found"};
inline constexpr ErrorDefinition ChallengeExpired{
    .code = ErrorCode::NotFound,
    .message = "Challenge expired"};
inline constexpr ErrorDefinition RefreshTokenInvalidOrExpired{
    .code = ErrorCode::Unauthorized,
    .message = "Invalid or expired refresh token"};
inline constexpr ErrorDefinition UserNotFound{
    .code = ErrorCode::NotFound,
    .message = "User not found"};
inline constexpr ErrorDefinition DeviceCredentialIssuanceFailed{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Failed to issue device credential"};
inline constexpr ErrorDefinition InvitationCreationFailed{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Unable to create invitation"};
inline constexpr ErrorDefinition InvitationResolutionFailed{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Unable to resolve invitation"};
inline constexpr ErrorDefinition InvitationOwnerAccessForbidden{
    .code = ErrorCode::BadRequest,
    .message = "Invitations cannot grant owner access"};
inline constexpr ErrorDefinition InvitationNotFound{
    .code = ErrorCode::NotFound,
    .message = "Invitation not found"};
inline constexpr ErrorDefinition ServerCertificateUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Server certificate is unavailable"};
inline constexpr ErrorDefinition PortraitPreviewPreparationFailed{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Unable to prepare portrait preview"};
inline constexpr ErrorDefinition PortraitVerificationUnavailable{
    .code = ErrorCode::Forbidden,
    .message = "Portrait verification is not available"};
inline constexpr ErrorDefinition InvalidUserId{
    .code = ErrorCode::BadRequest,
    .message = "Invalid user id"};
inline constexpr ErrorDefinition PortraitUnavailable{
    .code = ErrorCode::NotFound,
    .message = "Portrait is unavailable"};
inline constexpr ErrorDefinition PortraitPreviewUnavailable{
    .code = ErrorCode::NotFound,
    .message = "Portrait preview is unavailable"};
inline constexpr ErrorDefinition ActiveOwnerRequired{
    .code = ErrorCode::Conflict,
    .message = "At least one active owner is required"};
}
