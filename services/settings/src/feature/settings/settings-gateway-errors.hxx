#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace SettingsGatewayErrors
{
inline constexpr ErrorDefinition WriteFailed{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The settings owner could not save the change"};
inline constexpr ErrorDefinition UnknownProfile{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Unknown settings profile"};
inline constexpr ErrorDefinition ProfilesUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Settings profiles are not available"};
}
