#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

// What the HTTP substrate refuses on its own. These are the answers no handler
// produced and no caller threw -- an unmatched route, a request body the advice
// could not turn into a typed failure -- so nothing domain-shaped can name them.
// Every one of them still travels through ApiResponse (architecture plan
// section 4.7).
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
// The advice's last resort: an exception nobody typed. Its message is replaced
// with what the exception said, the code and the status are not.
inline constexpr ErrorDefinition InternalError{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "Internal error"};
} // namespace HttpErrors
