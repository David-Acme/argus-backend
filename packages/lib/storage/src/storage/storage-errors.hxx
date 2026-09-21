#pragma once

#include <errors/error-definition.hxx>
#include <errors/error-code.hxx>

namespace StorageErrors
{
inline constexpr ErrorDefinition InvalidObjectUpload{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid object upload"};
inline constexpr ErrorDefinition InvalidPortraitUpload{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid portrait upload"};
inline constexpr ErrorDefinition InvalidPortraitObject{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Invalid portrait object"};
}
