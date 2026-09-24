#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace LlmErrors
{
inline constexpr ErrorDefinition LlmEngineNotLoaded{
    .code = ErrorCode::LlmNotLoaded,
    .status = 503,
    .message = "LLM engine is not loaded"};
inline constexpr ErrorDefinition BodyNotJsonObject{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Body must be a JSON object"};
}
