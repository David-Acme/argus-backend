#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace VlmErrors
{
inline constexpr ErrorDefinition VisionEngineNotLoaded{
    .code = ErrorCode::VlmNotLoaded,
    .status = 503,
    .message = "Vision engine is not loaded"};
inline constexpr ErrorDefinition BodyNotJsonObject{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Body must be a JSON object"};
}
