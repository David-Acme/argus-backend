#pragma once

#include <error-definition.hxx>

namespace GatewayErrors
{
inline constexpr ErrorDefinition CameraStreamUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Camera stream unavailable"};
inline constexpr ErrorDefinition CameraStreamQueueOverflow{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Camera stream queue overflow"};
inline constexpr ErrorDefinition LegacySyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Legacy sync unavailable"};
inline constexpr ErrorDefinition CameraSyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Camera sync unavailable"};
inline constexpr ErrorDefinition NotificationSyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Notification sync unavailable"};
inline constexpr ErrorDefinition ProductivitySyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .message = "Productivity sync unavailable"};
}
