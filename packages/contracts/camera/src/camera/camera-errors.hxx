#pragma once

#include <errors/error-definition.hxx>
#include <errors/error-code.hxx>

namespace CameraErrors
{
inline constexpr ErrorDefinition Forbidden{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "Forbidden"};
inline constexpr ErrorDefinition InvalidCameraId{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid cameraId"};
inline constexpr ErrorDefinition CameraNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Camera not found"};
inline constexpr ErrorDefinition TooManyCameraSubscriptions{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Too many camera subscriptions"};
inline constexpr ErrorDefinition TooManyViewers{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "too_many_viewers"};
inline constexpr ErrorDefinition SubscribeFailed{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "subscribe_failed"};
inline constexpr ErrorDefinition ZoneNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Zone not found"};
inline constexpr ErrorDefinition CameraUnreachable{
    .code = ErrorCode::CameraUnreachable,
    .status = 502,
    .message = "The camera refused the command"};
inline constexpr ErrorDefinition ChangeNotRecorded{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The change could not be recorded"};
}
