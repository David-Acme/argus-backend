#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

// What the guard API refuses with. The guard domain has no contract package of
// its own yet (its clients are consumers, not callers of this vocabulary), so
// its three refusals live beside the controller that answers them; the day it
// gets a contract, this header moves there unchanged.
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
inline constexpr ErrorDefinition ExpectedGuestNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Expected guest not found"};
} // namespace GuardErrors
