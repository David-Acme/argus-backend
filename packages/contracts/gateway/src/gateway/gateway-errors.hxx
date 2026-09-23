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
}
