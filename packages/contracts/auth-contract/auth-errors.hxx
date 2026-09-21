#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

// The auth boundary's refusals, declared once for the three gates that answer
// with them (the JSON-body filter, the bearer-token filter and the role gate)
// and for the services whose own routes refuse the same way. A filter has no
// status code to pick: it throws one of these and the shared advice formats it.
namespace AuthErrors
{
// The token filter's four refusals: no token, a token that did not verify, a
// token that verified but carries no usable subject.
inline constexpr ErrorDefinition MissingToken{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Missing authorization token"};
inline constexpr ErrorDefinition AuthenticationRequired{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Authentication required"};
// The role gate: the subject is known and the path is not theirs.
inline constexpr ErrorDefinition AccessDenied{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "Access denied"};
// The body gate, ahead of any handler that would parse it.
inline constexpr ErrorDefinition InvalidJsonBody{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid JSON body"};
} // namespace AuthErrors
