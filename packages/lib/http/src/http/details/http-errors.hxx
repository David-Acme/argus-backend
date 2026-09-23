#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace HttpErrors
{
inline constexpr ErrorDefinition PathNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Path not found"};
inline constexpr ErrorDefinition MethodNotAllowed{
    .code = ErrorCode::MethodNotAllowed,
    .status = 405,
    .message = "Method not allowed"};
inline constexpr ErrorDefinition ValidationFailed{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "Validation failed"};
inline constexpr ErrorDefinition InternalError{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "Internal error"};
}
