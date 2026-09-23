#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace NotificationErrors
{
inline constexpr ErrorDefinition ChangeNotRecorded{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The change could not be recorded"};
}
