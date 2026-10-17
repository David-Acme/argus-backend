#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace SafetyErrors
{
inline constexpr ErrorDefinition PinRequired{.code = ErrorCode::PinRequired,
                                             .status = 403,
                                             .message = "A PIN is required to disarm"};
inline constexpr ErrorDefinition PinInvalid{.code = ErrorCode::PinInvalid,
                                            .status = 403,
                                            .message = "The PIN is not correct"};
inline constexpr ErrorDefinition PinLocked{.code = ErrorCode::PinLocked,
                                           .status = 429,
                                           .message = "Too many wrong PINs; try again later"};
inline constexpr ErrorDefinition DuressDisabled{.code = ErrorCode::Conflict,
                                                .status = 409,
                                                .message = "Disarm PINs are not enabled"};
inline constexpr ErrorDefinition AppSirenDisabled{
    .code = ErrorCode::AppSirenDisabled,
    .status = 403,
    .message = "The camera siren from the app is off"};
inline constexpr ErrorDefinition SirenUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "This home has no camera actions configured"};
inline constexpr ErrorDefinition MalformedBody{.code = ErrorCode::BadRequest,
                                              .status = 400,
                                              .message = "Invalid JSON body"};
inline constexpr ErrorDefinition PinsMustDiffer{.code = ErrorCode::ValidationError,
                                                .status = 422,
                                                .message = "The two PINs must be different"};
inline constexpr ErrorDefinition PhraseInvalid{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "The duress phrase must be 3 to 12 words without digits"};
}
