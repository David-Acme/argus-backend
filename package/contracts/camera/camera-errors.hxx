#pragma once

#include <error-definition.hxx>

namespace CameraErrors
{
inline constexpr ErrorDefinition Forbidden{
    .code = ErrorCode::Forbidden,
    .message = "Forbidden"};
inline constexpr ErrorDefinition InvalidCameraId{
    .code = ErrorCode::BadRequest,
    .message = "Invalid cameraId"};
inline constexpr ErrorDefinition CameraNotFound{
    .code = ErrorCode::NotFound,
    .message = "Camera not found"};
inline constexpr ErrorDefinition TooManyCameraSubscriptions{
    .code = ErrorCode::TooManyRequests,
    .message = "Too many camera subscriptions"};
inline constexpr ErrorDefinition TooManyViewers{
    .code = ErrorCode::TooManyRequests,
    .message = "too_many_viewers"};
inline constexpr ErrorDefinition SubscribeFailed{
    .code = ErrorCode::ServiceUnavailable,
    .message = "subscribe_failed"};
}
