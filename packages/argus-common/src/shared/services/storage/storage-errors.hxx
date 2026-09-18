#pragma once

#include <error-definition.hxx>

namespace StorageErrors
{
inline constexpr ErrorDefinition InvalidObjectUpload{
    .code = ErrorCode::BadRequest,
    .message = "Invalid object upload"};
inline constexpr ErrorDefinition InvalidPortraitUpload{
    .code = ErrorCode::BadRequest,
    .message = "Invalid portrait upload"};
inline constexpr ErrorDefinition InvalidPortraitObject{
    .code = ErrorCode::BadRequest,
    .message = "Invalid portrait object"};
}
