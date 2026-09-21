#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace TtsErrors
{
inline constexpr ErrorDefinition TtsNotLoaded{
    .code = ErrorCode::TtsNotLoaded,
    .status = 503,
    .message = "Text-to-speech engine is not loaded"};
inline constexpr ErrorDefinition InvalidRequest{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid synthesis request"};
inline constexpr ErrorDefinition Unauthorized{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Service credential required"};
inline constexpr ErrorDefinition Cancelled{
    .code = ErrorCode::Cancelled,
    .status = 499,
    .message = "Synthesis cancelled"};
inline constexpr ErrorDefinition DeadlineExceeded{
    .code = ErrorCode::DeadlineExceeded,
    .status = 504,
    .message = "Synthesis deadline exceeded"};
inline constexpr ErrorDefinition Busy{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Synthesis busy"};
inline constexpr ErrorDefinition InternalError{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "Synthesis failed"};
inline constexpr ErrorDefinition InvalidResponse{
    .code = ErrorCode::BadGateway,
    .status = 502,
    .message = "Invalid synthesis response"};
inline constexpr ErrorDefinition Unavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Text-to-speech service unavailable"};
} // namespace TtsErrors
