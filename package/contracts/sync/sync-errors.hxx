#pragma once

#include <error-definition.hxx>

namespace SyncErrors
{
inline constexpr ErrorDefinition UserAccountDisabled{
    .code = ErrorCode::Unauthorized,
    .message = "User account is disabled"};
inline constexpr ErrorDefinition MissingMessageType{
    .code = ErrorCode::BadRequest,
    .message = "Missing message type"};
inline constexpr ErrorDefinition UnknownMessageType{
    .code = ErrorCode::BadRequest,
    .message = "Unknown message type"};
inline constexpr ErrorDefinition NotificationSyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Notification sync unavailable"};
inline constexpr ErrorDefinition CameraSyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Camera sync unavailable"};
inline constexpr ErrorDefinition ProductivitySyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Productivity sync unavailable"};
}
