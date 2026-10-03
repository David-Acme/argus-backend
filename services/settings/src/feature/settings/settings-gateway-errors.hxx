#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace SettingsGatewayErrors
{
inline constexpr ErrorDefinition WriteFailed{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The settings owner could not save the change"};
}
