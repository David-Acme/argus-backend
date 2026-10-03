#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace SettingsErrors
{
inline constexpr ErrorDefinition Unauthorized{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Service credential required"};
inline constexpr ErrorDefinition InvalidRequest{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid settings request"};
inline constexpr ErrorDefinition Unavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Settings owner unavailable"};
inline constexpr ErrorDefinition UnknownService{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Unknown settings owner"};
}
