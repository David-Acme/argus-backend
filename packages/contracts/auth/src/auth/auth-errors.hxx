#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace AuthErrors
{
inline constexpr ErrorDefinition MissingToken{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Missing authorization token"};
inline constexpr ErrorDefinition AuthenticationRequired{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Authentication required"};
inline constexpr ErrorDefinition AccessDenied{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "Access denied"};
inline constexpr ErrorDefinition InvalidJsonBody{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid JSON body"};
}
