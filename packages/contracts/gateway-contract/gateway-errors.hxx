#pragma once

#include <errors/error-definition.hxx>
#include <errors/error-code.hxx>

namespace GatewayErrors
{
inline constexpr ErrorDefinition CameraStreamUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Camera stream unavailable"};
inline constexpr ErrorDefinition CameraStreamQueueOverflow{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Camera stream queue overflow"};
inline constexpr ErrorDefinition LegacySyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Legacy sync unavailable"};
inline constexpr ErrorDefinition CameraSyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Camera sync unavailable"};
inline constexpr ErrorDefinition NotificationSyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Notification sync unavailable"};
inline constexpr ErrorDefinition ProductivitySyncUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Productivity sync unavailable"};
// The three answers the gateway's own gates give: a remote caller on a LAN-only
// path, a caller that has worn out the rate limiter, and a route whose backend
// did not answer at all.
inline constexpr ErrorDefinition RemoteNotAllowed{
    .code = ErrorCode::RemoteNotAllowed,
    .status = 403,
    .message = "Remote requests are not allowed"};
inline constexpr ErrorDefinition TooManyRemoteAttempts{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Too many requests"};
inline constexpr ErrorDefinition RouteUnreachable{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "Route backend is unreachable"};
} // namespace GatewayErrors
