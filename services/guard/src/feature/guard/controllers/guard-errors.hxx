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
inline constexpr ErrorDefinition EnvironmentNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Environment not found"};
inline constexpr ErrorDefinition EnvironmentNameTaken{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "Another environment already has that name"};
inline constexpr ErrorDefinition DefaultEnvironmentKept{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "The default environment cannot be removed"};
inline constexpr ErrorDefinition ExpectedGuestNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Expected guest not found"};
inline constexpr ErrorDefinition GuestWindowTooLong{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "An expected visit lasts 24 hours at most"};
inline constexpr ErrorDefinition GuestScopeRequired{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "An expected visit needs a person, or a camera and an environment"};
inline constexpr ErrorDefinition GuestHostNotCaller{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "Only the owner names another host"};
inline constexpr ErrorDefinition RecipientUnknown{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "A recipient is not an active user of this installation"};
inline constexpr ErrorDefinition DirectoryUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "The user directory is unavailable"};
inline constexpr ErrorDefinition DutyForGuardsOnly{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "Only a guard goes on duty"};
}
