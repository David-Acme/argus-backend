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
inline constexpr ErrorDefinition PinsMustDiffer{.code = ErrorCode::ValidationError,
                                                .status = 422,
                                                .message = "The two PINs must be different"};
}
