#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace RtcErrors
{
inline constexpr ErrorDefinition RtcUnavailable{
    .code = ErrorCode::RtcUnavailable,
    .status = 503,
    .message = "Realtime calls are unavailable"};
inline constexpr ErrorDefinition CallNotFound{
    .code = ErrorCode::CallNotFound,
    .status = 404,
    .message = "Call not found"};
inline constexpr ErrorDefinition CallTaken{
    .code = ErrorCode::CallTaken,
    .status = 409,
    .message = "The call was answered on another device"};
inline constexpr ErrorDefinition TooManyCalls{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Too many calls at once"};
inline constexpr ErrorDefinition CallExpired{
    .code = ErrorCode::CallExpired,
    .status = 410,
    .message = "The call has expired"};
inline constexpr ErrorDefinition WatchNotFound{
    .code = ErrorCode::WatchNotFound,
    .status = 404,
    .message = "No camera conversation of yours by that id"};
inline constexpr ErrorDefinition WatchExpired{
    .code = ErrorCode::WatchExpired,
    .status = 410,
    .message = "The camera conversation can no longer be continued"};
inline constexpr ErrorDefinition WatchVisitorNotFound{
    .code = ErrorCode::WatchNotFound,
    .status = 404,
    .message = "No visitor's message to you by that id"};
inline constexpr ErrorDefinition WatchReplyClosed{
    .code = ErrorCode::WatchExpired,
    .status = 410,
    .message = "The visitor's message can no longer be answered"};
inline constexpr ErrorDefinition WatchReplyTooLong{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "The reply is longer than a camera may say"};
}
