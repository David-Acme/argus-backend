#pragma once

#include <errors/error-definition.hxx>
#include <errors/error-code.hxx>

namespace IdentityErrors
{
inline constexpr ErrorDefinition FaceNotRecognized{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Face not recognized"};
inline constexpr ErrorDefinition ServerNotPaired{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "Server is not paired yet"};
inline constexpr ErrorDefinition FaceExtractionFailed{
    .code = ErrorCode::BadRequest,
    .status = 422,
    .message = "Face could not be extracted"};
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
inline constexpr ErrorDefinition LoginChallengeGenerationFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 500,
    .message = "Failed to generate login challenge"};
inline constexpr ErrorDefinition ChallengeNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Challenge not found"};
inline constexpr ErrorDefinition ChallengeExpired{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Challenge expired"};
inline constexpr ErrorDefinition RefreshTokenInvalidOrExpired{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Invalid or expired refresh token"};
inline constexpr ErrorDefinition UserNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "User not found"};
inline constexpr ErrorDefinition DeviceCredentialIssuanceFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 500,
    .message = "Failed to issue device credential"};
inline constexpr ErrorDefinition InvitationCreationFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Unable to create invitation"};
inline constexpr ErrorDefinition InvitationResolutionFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Unable to resolve invitation"};
inline constexpr ErrorDefinition InvitationOwnerAccessForbidden{
    .code = ErrorCode::BadRequest,
    .status = 422,
    .message = "Invitations cannot grant owner access"};
inline constexpr ErrorDefinition InvitationRoleInactive{
    .code = ErrorCode::RoleInactive,
    .status = 409,
    .message = "The module that brings this role is not active"};
inline constexpr ErrorDefinition InvitationNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Invitation not found"};
inline constexpr ErrorDefinition ServerCertificateUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Server certificate is unavailable"};
inline constexpr ErrorDefinition PortraitPreviewPreparationFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Unable to prepare portrait preview"};
inline constexpr ErrorDefinition PortraitVerificationUnavailable{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "Portrait verification is not available"};
inline constexpr ErrorDefinition InvalidUserId{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid user id"};
inline constexpr ErrorDefinition PortraitUnavailable{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Portrait is unavailable"};
inline constexpr ErrorDefinition PortraitPreviewUnavailable{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Portrait preview is unavailable"};
inline constexpr ErrorDefinition ActiveOwnerRequired{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "At least one active owner is required"};
inline constexpr ErrorDefinition SelfDeactivationForbidden{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "You cannot deactivate your own account"};
inline constexpr ErrorDefinition InvalidMultipartForm{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid multipart form"};
inline constexpr ErrorDefinition MissingChallengeId{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Missing challenge id"};
inline constexpr ErrorDefinition InvalidInvitationId{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid invitation id"};
inline constexpr ErrorDefinition InvalidPairingCode{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "Invalid pairing code"};
inline constexpr ErrorDefinition ChangeNotRecorded{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The change could not be recorded"};
inline constexpr ErrorDefinition PrivacyNoticeOutdated{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "The privacy notice changed; review it again"};
inline constexpr ErrorDefinition VoiceprintNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Argus has not learned this person's voice"};
inline constexpr ErrorDefinition VisitorNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "This person is not in the gallery"};
inline constexpr ErrorDefinition VisitorSampleNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "This face sample is not part of the person"};
inline constexpr ErrorDefinition VisitorMergeInvalid{
    .code = ErrorCode::BadRequest,
    .status = 422,
    .message = "Choose other people to merge into this one"};
inline constexpr ErrorDefinition VisitorSplitInvalid{
    .code = ErrorCode::BadRequest,
    .status = 422,
    .message = "Choose some, but not all, of the person's face samples"};
inline constexpr ErrorDefinition VisitorCropUnavailable{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "The face picture is unavailable"};
inline constexpr ErrorDefinition VisitorRecognitionOff{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "Recognition of recurring visitors is turned off"};
inline constexpr ErrorDefinition LivenessCheckFailed{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "The face did not pass the liveness check"};
inline constexpr ErrorDefinition LivenessUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "The liveness check is unavailable"};
inline constexpr ErrorDefinition FaceQualityInsufficient{
    .code = ErrorCode::BadRequest,
    .status = 422,
    .message = "Retake the photo: one face, close to the camera, facing it and in focus"};
inline constexpr ErrorDefinition TooManyAttempts{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Too many attempts; try again later"};
inline constexpr ErrorDefinition BiometricEraseOwnerOnly{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "Only an owner can erase biometric data"};
}
