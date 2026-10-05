#pragma once

#include <errors/error-definition.hxx>
#include <errors/error-code.hxx>

namespace SyncErrors
{
inline constexpr ErrorDefinition UserAccountDisabled{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "User account is disabled"};
inline constexpr ErrorDefinition MissingMessageType{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Missing message type"};
inline constexpr ErrorDefinition InvalidPayload{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Message payload must be an object"};
inline constexpr ErrorDefinition UnknownMessageType{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Unknown message type"};
inline constexpr ErrorDefinition NotificationSyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Notification sync unavailable"};
inline constexpr ErrorDefinition CameraSyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Camera sync unavailable"};
inline constexpr ErrorDefinition ProductivitySyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Productivity sync unavailable"};
inline constexpr ErrorDefinition IdentitySyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Identity sync unavailable"};
inline constexpr ErrorDefinition VoiceUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Voice unavailable"};
inline constexpr ErrorDefinition FrameFailed{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The message could not be handled"};
inline constexpr ErrorDefinition TooManyFrames{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Too many messages on this socket"};
inline constexpr ErrorDefinition ReplicaTooOld{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "Audit cursor is older than the retention window"};
}
