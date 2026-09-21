#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

// What the vision API refuses with. The engine-not-loaded answer is the
// service's own status, not a domain error of anybody else's, so it lives
// beside the controller that answers it.
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
} // namespace VlmErrors
