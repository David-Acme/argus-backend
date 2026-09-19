#pragma once

#include <error-definition.hxx>

namespace TtsErrors
{
inline constexpr ErrorDefinition TtsNotLoaded{
    .code = ErrorCode::TtsNotLoaded,
    .message = "Text-to-speech engine is not loaded"};
inline constexpr ErrorDefinition InvalidRequest{
    .code = ErrorCode::BadRequest, .message = "Invalid synthesis request"};
inline constexpr ErrorDefinition Unauthorized{
    .code = ErrorCode::Unauthorized, .message = "Service credential required"};
inline constexpr ErrorDefinition Cancelled{
    .code = ErrorCode::Cancelled, .message = "Synthesis cancelled"};
inline constexpr ErrorDefinition DeadlineExceeded{
    .code = ErrorCode::DeadlineExceeded, .message = "Synthesis deadline exceeded"};
inline constexpr ErrorDefinition Busy{
    .code = ErrorCode::TooManyRequests, .message = "Synthesis busy"};
inline constexpr ErrorDefinition InternalError{
    .code = ErrorCode::InternalError, .message = "Synthesis failed"};
inline constexpr ErrorDefinition InvalidResponse{
    .code = ErrorCode::BadGateway, .message = "Invalid synthesis response"};
inline constexpr ErrorDefinition Unavailable{
    .code = ErrorCode::ServiceUnavailable, .message = "Text-to-speech service unavailable"};
}
