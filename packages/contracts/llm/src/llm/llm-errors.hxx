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
inline constexpr ErrorDefinition InvalidRequest{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid chat request"};
inline constexpr ErrorDefinition Unauthorized{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Service credential required"};
inline constexpr ErrorDefinition Cancelled{
    .code = ErrorCode::Cancelled,
    .status = 499,
    .message = "Chat generation cancelled"};
inline constexpr ErrorDefinition DeadlineExceeded{
    .code = ErrorCode::DeadlineExceeded,
    .status = 504,
    .message = "Chat generation deadline exceeded"};
inline constexpr ErrorDefinition Busy{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Chat generation busy"};
inline constexpr ErrorDefinition InternalError{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "Chat generation failed"};
inline constexpr ErrorDefinition InvalidResponse{
    .code = ErrorCode::BadGateway,
    .status = 502,
    .message = "Invalid chat response"};
inline constexpr ErrorDefinition Unavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Chat service unavailable"};
}
