#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace GuardErrors
{
inline constexpr ErrorDefinition OwnerAccessTokenRequired{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Owner access token required"};
inline constexpr ErrorDefinition DecisionNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Decision not found"};
inline constexpr ErrorDefinition EpisodeNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Episode not found"};
inline constexpr ErrorDefinition CameraIdInvalid{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Camera id must be positive"};
inline constexpr ErrorDefinition ExpectedGuestNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Expected guest not found"};
}
