#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace SttErrors
{
inline constexpr ErrorDefinition SpeechEngineNotLoaded{
    .code = ErrorCode::SttNotLoaded,
    .status = 503,
    .message = "Speech-to-text engine is not loaded"};
inline constexpr ErrorDefinition InvalidRequest{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid transcription request"};
inline constexpr ErrorDefinition BodyNotPcmS16{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Body must be audio/x-argus-pcm-s16"};
inline constexpr ErrorDefinition PcmBodyMisaligned{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "PCM body is not int16-aligned"};
inline constexpr ErrorDefinition Unauthorized{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Service credential required"};
inline constexpr ErrorDefinition Cancelled{
    .code = ErrorCode::Cancelled,
    .status = 499,
    .message = "Transcription cancelled"};
inline constexpr ErrorDefinition DeadlineExceeded{
    .code = ErrorCode::DeadlineExceeded,
    .status = 504,
    .message = "Transcription deadline exceeded"};
inline constexpr ErrorDefinition Busy{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Transcription busy"};
inline constexpr ErrorDefinition InternalError{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "Transcription failed"};
inline constexpr ErrorDefinition InvalidResponse{
    .code = ErrorCode::BadGateway,
    .status = 502,
    .message = "Invalid transcription response"};
inline constexpr ErrorDefinition Unavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Speech-to-text service unavailable"};
}
