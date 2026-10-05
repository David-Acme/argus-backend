#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace NotificationErrors
{
inline constexpr ErrorDefinition ChangeNotRecorded{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The change could not be recorded"};
inline constexpr ErrorDefinition ResponseNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Response not found"};
inline constexpr ErrorDefinition ResponseClosed{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "The response already has another verdict"};
}
