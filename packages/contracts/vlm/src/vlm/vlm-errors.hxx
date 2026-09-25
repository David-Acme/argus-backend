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
inline constexpr ErrorDefinition InvalidRequest{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid vision request"};
inline constexpr ErrorDefinition ImageNotDecodable{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Image is not decodable"};
inline constexpr ErrorDefinition Unauthorized{
    .code = ErrorCode::Unauthorized,
    .status = 401,
    .message = "Service credential required"};
inline constexpr ErrorDefinition Cancelled{
    .code = ErrorCode::Cancelled,
    .status = 499,
    .message = "Vision caption cancelled"};
inline constexpr ErrorDefinition DeadlineExceeded{
    .code = ErrorCode::DeadlineExceeded,
    .status = 504,
    .message = "Vision caption deadline exceeded"};
inline constexpr ErrorDefinition Busy{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Vision caption busy"};
inline constexpr ErrorDefinition InternalError{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "Vision caption failed"};
inline constexpr ErrorDefinition InvalidResponse{
    .code = ErrorCode::BadGateway,
    .status = 502,
    .message = "Invalid vision response"};
inline constexpr ErrorDefinition Unavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "Vision service unavailable"};
}
